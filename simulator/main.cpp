#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " failed (HRESULT " + std::to_string(hr) + ")");
}

struct Vertex { XMFLOAT3 position; XMFLOAT3 normal; };
struct alignas(16) FrameConstants
{
    XMFLOAT4X4 viewProjection;
    XMFLOAT4X4 view;
    XMFLOAT4 cameraTime;
    XMFLOAT4 viewportNearFar;
    XMFLOAT4 water; // height, amplitude, frequency, speed
    XMFLOAT4 ocean; // roughness, storm blend, exposure, whitecap strength
    XMFLOAT4 simulation; // fixed dt, simulation span, resolution, wind strength
    XMFLOAT4 wind; // direction x/z, gust, reserved
};
static_assert(sizeof(FrameConstants) == 224);

struct SprayParticle { XMFLOAT4 positionAge, velocityLife; };

class Renderer
{
public:
    HWND window = nullptr;
    UINT width = 1280, height = 720;
    bool minimized = false;
    float yaw = 0.65f, pitch = 0.20f, distance = 32.0f;
    bool storm = false;
    bool showSpray = true, showFoam = true;
    bool recordPreview = false;

    void Initialize()
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = width;
        desc.BufferDesc.Height = height;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL level;
        const D3D_FEATURE_LEVEL requested[] = { D3D_FEATURE_LEVEL_11_0 };
        auto create = [&](D3D_DRIVER_TYPE driver, UINT creationFlags) {
            return D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, creationFlags,
                requested, 1, D3D11_SDK_VERSION, &desc, &swapChain, &device, &level, &context);
        };
        HRESULT hr = create(D3D_DRIVER_TYPE_HARDWARE, flags);
        if (FAILED(hr)) hr = create(D3D_DRIVER_TYPE_HARDWARE, 0);
        if (FAILED(hr)) hr = create(D3D_DRIVER_TYPE_WARP, 0);
        Check(hr, "Create D3D11 device");
        ComPtr<IDXGIFactory> factory;
        Check(swapChain->GetParent(IID_PPV_ARGS(&factory)), "Get DXGI factory");
        Check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "Disable automatic fullscreen");

        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        shaderPath = std::filesystem::path(executable).parent_path() / L"water.fx";
        ReloadShaders();
        CreateGeometry();
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(FrameConstants);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&buffer, nullptr, &constants), "Create constants");

        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        Check(device->CreateRasterizerState(&raster, &rasterizer), "Create rasterizer");
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        Check(device->CreateDepthStencilState(&depth, &sceneDepthState), "Create scene depth state");
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        Check(device->CreateDepthStencilState(&depth, &waterDepthState), "Create water depth state");
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&sampler, &linearSampler), "Create sampler");
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(device->CreateBlendState(&blend, &sprayBlend), "Create spray blend");
        CreateSimulation();
        Resize(width, height);
    }

    void ReloadShaders()
    {
        // Build a complete replacement before touching the active pipeline.
        auto compile = [&](const char* entry, const char* target) {
            ComPtr<ID3DBlob> bytecode, errors;
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
            HRESULT hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                entry, target, flags, 0, &bytecode, &errors);
            if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';
            Check(hr, entry);
            return bytecode;
        };
        auto sceneVSCode = compile("SceneVS", "vs_5_0");
        auto scenePSCode = compile("ScenePS", "ps_5_0");
        auto waterVSCode = compile("WaterVS", "vs_5_0");
        auto waterPSCode = compile("WaterPS", "ps_5_0");
        auto fullscreenCode = compile("FullscreenVS", "vs_5_0");
        auto skyCode = compile("SkyPS", "ps_5_0");
        auto toneCode = compile("TonePS", "ps_5_0");
        auto oceanCSCode = compile("OceanCS", "cs_5_0");
        auto sprayCSCode = compile("SprayCS", "cs_5_0");
        auto sprayVSCode = compile("SprayVS", "vs_5_0");
        auto sprayPSCode = compile("SprayPS", "ps_5_0");
        ComPtr<ID3D11VertexShader> newSceneVS, newWaterVS;
        ComPtr<ID3D11PixelShader> newScenePS, newWaterPS;
        ComPtr<ID3D11InputLayout> newLayout;
        ComPtr<ID3D11VertexShader> newFullscreenVS;
        ComPtr<ID3D11PixelShader> newSkyPS, newTonePS;
        ComPtr<ID3D11ComputeShader> newOceanCS, newSprayCS;
        ComPtr<ID3D11VertexShader> newSprayVS;
        ComPtr<ID3D11PixelShader> newSprayPS;
        Check(device->CreateVertexShader(sceneVSCode->GetBufferPointer(), sceneVSCode->GetBufferSize(), nullptr, &newSceneVS), "Scene VS");
        Check(device->CreateVertexShader(waterVSCode->GetBufferPointer(), waterVSCode->GetBufferSize(), nullptr, &newWaterVS), "Water VS");
        Check(device->CreatePixelShader(scenePSCode->GetBufferPointer(), scenePSCode->GetBufferSize(), nullptr, &newScenePS), "Scene PS");
        Check(device->CreatePixelShader(waterPSCode->GetBufferPointer(), waterPSCode->GetBufferSize(), nullptr, &newWaterPS), "Water PS");
        Check(device->CreateVertexShader(fullscreenCode->GetBufferPointer(), fullscreenCode->GetBufferSize(), nullptr, &newFullscreenVS), "Fullscreen VS");
        Check(device->CreatePixelShader(skyCode->GetBufferPointer(), skyCode->GetBufferSize(), nullptr, &newSkyPS), "Sky PS");
        Check(device->CreatePixelShader(toneCode->GetBufferPointer(), toneCode->GetBufferSize(), nullptr, &newTonePS), "Tone PS");
        Check(device->CreateComputeShader(oceanCSCode->GetBufferPointer(), oceanCSCode->GetBufferSize(), nullptr, &newOceanCS), "Ocean CS");
        Check(device->CreateComputeShader(sprayCSCode->GetBufferPointer(), sprayCSCode->GetBufferSize(), nullptr, &newSprayCS), "Spray CS");
        Check(device->CreateVertexShader(sprayVSCode->GetBufferPointer(), sprayVSCode->GetBufferSize(), nullptr, &newSprayVS), "Spray VS");
        Check(device->CreatePixelShader(sprayPSCode->GetBufferPointer(), sprayPSCode->GetBufferSize(), nullptr, &newSprayPS), "Spray PS");
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
        };
        Check(device->CreateInputLayout(elements, 2, sceneVSCode->GetBufferPointer(), sceneVSCode->GetBufferSize(), &newLayout), "Input layout");
        sceneVS = newSceneVS; waterVS = newWaterVS;
        scenePS = newScenePS; waterPS = newWaterPS; layout = newLayout;
        fullscreenVS = newFullscreenVS; skyPS = newSkyPS; tonePS = newTonePS;
        oceanCS = newOceanCS; sprayCS = newSprayCS; sprayVS = newSprayVS; sprayPS = newSprayPS;
        std::cout << "Shaders loaded: " << shaderPath.string() << '\n';
    }

    void Resize(UINT newWidth, UINT newHeight)
    {
        if (!context || newWidth == 0 || newHeight == 0) return;
        width = newWidth; height = newHeight;
        context->ClearState();
        backBuffer.Reset(); sceneColor.Reset(); sceneRTV.Reset(); sceneSRV.Reset();
        sceneDepth.Reset(); sceneDSV.Reset(); depthSRV.Reset();
        compositeColor.Reset(); compositeRTV.Reset(); compositeSRV.Reset();
        compositeDepth.Reset(); compositeDSV.Reset(); sprayDSV.Reset(); compositeDepthSRV.Reset();
        Check(swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "Resize swap chain");
        ComPtr<ID3D11Texture2D> back;
        Check(swapChain->GetBuffer(0, IID_PPV_ARGS(&back)), "Get back buffer");
        Check(device->CreateRenderTargetView(back.Get(), nullptr, &backBuffer), "Back buffer RTV");
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = width; texture.Height = height;
        texture.MipLevels = texture.ArraySize = 1;
        texture.SampleDesc.Count = 1;
        texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&texture, nullptr, &sceneColor), "Scene color");
        Check(device->CreateRenderTargetView(sceneColor.Get(), nullptr, &sceneRTV), "Scene RTV");
        Check(device->CreateShaderResourceView(sceneColor.Get(), nullptr, &sceneSRV), "Scene color SRV");
        Check(device->CreateTexture2D(&texture, nullptr, &compositeColor), "Composite color");
        Check(device->CreateRenderTargetView(compositeColor.Get(), nullptr, &compositeRTV), "Composite RTV");
        Check(device->CreateShaderResourceView(compositeColor.Get(), nullptr, &compositeSRV), "Composite SRV");
        texture.Format = DXGI_FORMAT_R32_TYPELESS;
        texture.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&texture, nullptr, &sceneDepth), "Scene depth");
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        Check(device->CreateDepthStencilView(sceneDepth.Get(), &dsv, &sceneDSV), "Scene DSV");
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        Check(device->CreateShaderResourceView(sceneDepth.Get(), &srv, &depthSRV), "Depth SRV");
        Check(device->CreateTexture2D(&texture, nullptr, &compositeDepth), "Composite depth");
        dsv.Flags = 0;
        Check(device->CreateDepthStencilView(compositeDepth.Get(), &dsv, &compositeDSV), "Composite DSV");
        dsv.Flags = D3D11_DSV_READ_ONLY_DEPTH;
        Check(device->CreateDepthStencilView(compositeDepth.Get(), &dsv, &sprayDSV), "Spray read-only DSV");
        Check(device->CreateShaderResourceView(compositeDepth.Get(), &srv, &compositeDepthSRV), "Composite depth SRV");
    }

    void Render(float seconds, bool capture = false)
    {
        XMVECTOR eye = XMVectorSet(distance * std::cos(pitch) * std::sin(yaw),
            distance * std::sin(pitch), distance * std::cos(pitch) * std::cos(yaw), 1);
        XMMATRIX view = XMMatrixLookAtLH(eye, XMVectorZero(), XMVectorSet(0, 1, 0, 0));
        XMMATRIX projection = XMMatrixPerspectiveFovLH(XM_PIDIV4, float(width) / height, 0.15f, 800.0f);
        FrameConstants frame{};
        XMStoreFloat4x4(&frame.viewProjection, view * projection);
        XMStoreFloat4x4(&frame.view, view);
        XMStoreFloat4(&frame.cameraTime, eye); frame.cameraTime.w = seconds;
        frame.viewportNearFar = XMFLOAT4(float(width), float(height), 0.15f, 800.0f);
        frame.water = XMFLOAT4(0.0f, storm ? 1.65f : 0.85f, 1.0f, storm ? 1.25f : 1.0f);
        frame.ocean = XMFLOAT4(storm ? 0.24f : 0.16f, storm ? 1.0f : 0.0f, 1.0f, storm ? 0.9f : 0.48f);
        frame.simulation = XMFLOAT4(1.0f / 60, 192, float(simulationResolution), storm ? 1.5f : 1.0f);
        AdvanceSimulation(seconds, frame);
        frame.cameraTime.w = simulationTime;
        const float windAngle = 0.4f + 0.38f * std::sin(simulationTime * 0.09f) + 0.2f * std::sin(simulationTime * 0.037f);
        frame.wind = XMFLOAT4(std::cos(windAngle), std::sin(windAngle), 1, showFoam ? 1.0f : 0.0f);
        context->UpdateSubresource(constants.Get(), 0, nullptr, &frame, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer.Get());
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* cb = constants.Get();
        context->VSSetConstantBuffers(0, 1, &cb); context->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* sampler = linearSampler.Get();
        context->PSSetSamplers(0, 1, &sampler);
        context->VSSetSamplers(0, 1, &sampler);
        const float sky[] = {0.38f, 0.60f, 0.78f, 1};

        // 1. Opaque geometry -> sampleable scene color and depth.
        ID3D11ShaderResourceView* empty[6]{};
        context->PSSetShaderResources(0, 6, empty);
        context->VSSetShaderResources(0, 6, empty);
        ID3D11RenderTargetView* target = sceneRTV.Get();
        context->OMSetRenderTargets(1, &target, sceneDSV.Get());
        context->OMSetDepthStencilState(sceneDepthState.Get(), 0);
        context->ClearRenderTargetView(target, sky);
        context->ClearDepthStencilView(sceneDSV.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        Fullscreen(skyPS.Get());
        context->IASetInputLayout(layout.Get());
        Draw(terrainVB.Get(), terrainIB.Get(), terrainIndexCount, sceneVS.Get(), scenePS.Get());

        // 2. Preserve opaque HDR color in a separate water composition target.
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(compositeColor.Get(), sceneColor.Get());
        context->CopyResource(compositeDepth.Get(), sceneDepth.Get());

        // 3. Water reads scene color/depth while testing against read-only depth.
        target = compositeRTV.Get();
        context->OMSetRenderTargets(1, &target, compositeDSV.Get());
        context->OMSetDepthStencilState(sceneDepthState.Get(), 0);
        ID3D11ShaderResourceView* resources[] = {sceneSRV.Get(), depthSRV.Get()};
        context->PSSetShaderResources(0, 2, resources);
        ID3D11ShaderResourceView* state = stateSRV[stateIndex].Get();
        context->VSSetShaderResources(2, 1, &state);
        context->PSSetShaderResources(2, 1, &state);
        Draw(waterVB.Get(), waterIB.Get(), waterIndexCount, waterVS.Get(), waterPS.Get());
        context->PSSetShaderResources(0, 6, empty);
        context->VSSetShaderResources(0, 6, empty);
        // Spray uses the water-inclusive depth, preventing droplets behind a crest from showing through.
        if (showSpray) {
            context->OMSetRenderTargets(1, &target, sprayDSV.Get());
            context->OMSetDepthStencilState(waterDepthState.Get(), 0);
            context->OMSetBlendState(sprayBlend.Get(), nullptr, 0xffffffff);
            context->IASetInputLayout(nullptr);
            ID3D11ShaderResourceView* particles = particlesSRV.Get();
            ID3D11ShaderResourceView* softDepth = compositeDepthSRV.Get();
            context->VSSetShaderResources(4, 1, &particles);
            context->PSSetShaderResources(5, 1, &softDepth);
            context->VSSetShader(sprayVS.Get(), nullptr, 0);
            context->PSSetShader(sprayPS.Get(), nullptr, 0);
            context->DrawInstanced(6, particleCount, 0, 0);
            context->PSSetShaderResources(0, 6, empty);
            context->VSSetShaderResources(0, 6, empty);
            context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        }
        // 4. HDR -> tone mapping -> display encoding. No depth needed.
        target = backBuffer.Get();
        context->OMSetRenderTargets(1, &target, nullptr);
        ID3D11ShaderResourceView* hdr = compositeSRV.Get();
        context->PSSetShaderResources(0, 1, &hdr);
        Fullscreen(tonePS.Get());
        context->PSSetShaderResources(0, 6, empty);
        if (capture) CaptureFrame();
        Check(swapChain->Present(1, 0), "Present");
    }

    void ValidateDebugMessages()
    {
        ComPtr<ID3D11InfoQueue> queue;
        if (FAILED(device.As(&queue))) {
            std::cout << "D3D11 debug layer unavailable; runtime checks only.\n";
            return;
        }
        bool failed = false;
        for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            Check(queue->GetMessage(i, nullptr, &size), "Debug message size");
            std::vector<unsigned char> storage(size);
            auto message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            Check(queue->GetMessage(i, message, &size), "Debug message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::cerr << message->pDescription << '\n';
                failed = true;
            }
        }
        if (failed) throw std::runtime_error("D3D11 validation reported warnings or errors");
        std::cout << "D3D11 validation: no warnings or errors.\n";
    }

    void ValidateSimulation()
    {
        D3D11_TEXTURE2D_DESC desc{};
        stateTexture[stateIndex]->GetDesc(&desc);
        desc.BindFlags = desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        Check(device->CreateTexture2D(&desc, nullptr, &readback), "Simulation readback");
        context->CopyResource(readback.Get(), stateTexture[stateIndex].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Simulation readback map");
        float maxFoam = 0, maxHeight = 0; bool finite = true;
        for (UINT y = 0; y < simulationResolution; ++y) {
            auto row = reinterpret_cast<const XMFLOAT4*>(static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch);
            for (UINT x = 0; x < simulationResolution; ++x) {
                const auto& value = row[x];
                finite &= std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
                maxHeight = std::max(maxHeight, std::abs(value.x)); maxFoam = std::max(maxFoam, value.z);
            }
        }
        context->Unmap(readback.Get(), 0);
        D3D11_BUFFER_DESC bufferDesc{};
        particlesBuffer->GetDesc(&bufferDesc);
        bufferDesc.BindFlags = bufferDesc.MiscFlags = bufferDesc.StructureByteStride = 0;
        bufferDesc.Usage = D3D11_USAGE_STAGING; bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> particleReadback;
        Check(device->CreateBuffer(&bufferDesc, nullptr, &particleReadback), "Particle readback");
        context->CopyResource(particleReadback.Get(), particlesBuffer.Get());
        Check(context->Map(particleReadback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Particle readback map");
        auto particles = static_cast<const SprayParticle*>(mapped.pData);
        UINT alive = 0;
        for (UINT i = 0; i < particleCount; ++i) {
            const auto& p = particles[i];
            finite &= std::isfinite(p.positionAge.x) && std::isfinite(p.positionAge.y) && std::isfinite(p.positionAge.z)
                && std::isfinite(p.positionAge.w) && std::isfinite(p.velocityLife.x) && std::isfinite(p.velocityLife.y)
                && std::isfinite(p.velocityLife.z) && std::isfinite(p.velocityLife.w);
            if (p.velocityLife.w > p.positionAge.w) ++alive;
        }
        context->Unmap(particleReadback.Get(), 0);
        std::cout << "Simulation t=" << simulationTime << ": max displacement=" << maxHeight
            << ", foam=" << maxFoam << ", live spray=" << alive << '\n';
        if (!finite || maxHeight >= 1.49f || maxFoam < 0.001f || maxFoam > 1.001f || alive == 0)
            throw std::runtime_error("Simulation validation failed: non-finite/unbounded state or no breaking effects");
    }

    void RunSimulationTest()
    {
        FrameConstants frame{};
        frame.water = XMFLOAT4(0, storm ? 1.65f : 0.85f, 1, storm ? 1.25f : 1);
        frame.ocean = XMFLOAT4(0.2f, storm ? 1.0f : 0.0f, 1, storm ? 0.9f : 0.48f);
        frame.simulation = XMFLOAT4(1.0f / 60, 192, float(simulationResolution), storm ? 1.5f : 1);
        for (UINT i = 0; i < 3600; ++i) {
            AdvanceSimulation(i / 60.0f, frame);
            if (i == 179 || i == 1199 || i == 3599) ValidateSimulation();
        }
        ValidateDebugMessages();
    }

private:
    static constexpr UINT simulationResolution = 256, particleCount = 4096;
    ComPtr<ID3D11Texture2D> stateTexture[2], compositeDepth;
    ComPtr<ID3D11ShaderResourceView> stateSRV[2], particlesSRV, compositeDepthSRV;
    ComPtr<ID3D11UnorderedAccessView> stateUAV[2], particlesUAV;
    ComPtr<ID3D11Buffer> particlesBuffer, impactBuffer;
    ComPtr<ID3D11UnorderedAccessView> impactUAV;
    ComPtr<ID3D11ComputeShader> oceanCS, sprayCS;
    ComPtr<ID3D11VertexShader> sprayVS;
    ComPtr<ID3D11PixelShader> sprayPS;
    ComPtr<ID3D11BlendState> sprayBlend;
    ComPtr<ID3D11DepthStencilView> compositeDSV, sprayDSV;
    UINT stateIndex = 0;
    float simulationTime = 0, accumulator = 0, previousSeconds = -1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> backBuffer, sceneRTV;
    ComPtr<ID3D11Texture2D> sceneColor, sceneDepth;
    ComPtr<ID3D11Texture2D> compositeColor;
    ComPtr<ID3D11RenderTargetView> compositeRTV;
    ComPtr<ID3D11ShaderResourceView> compositeSRV;
    ComPtr<ID3D11ShaderResourceView> sceneSRV, depthSRV;
    ComPtr<ID3D11DepthStencilView> sceneDSV;
    ComPtr<ID3D11VertexShader> sceneVS, waterVS;
    ComPtr<ID3D11PixelShader> scenePS, waterPS;
    ComPtr<ID3D11VertexShader> fullscreenVS;
    ComPtr<ID3D11PixelShader> skyPS, tonePS;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants, terrainVB, terrainIB, waterVB, waterIB;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> sceneDepthState, waterDepthState;
    ComPtr<ID3D11SamplerState> linearSampler;
    UINT terrainIndexCount = 0, waterIndexCount = 0;
    std::filesystem::path shaderPath;
    UINT previewFrame = 0;

    void CreateSimulation()
    {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = texture.Height = simulationResolution;
        texture.MipLevels = texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        texture.SampleDesc.Count = 1;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        const float zero[4]{};
        for (UINT i = 0; i < 2; ++i) {
            Check(device->CreateTexture2D(&texture, nullptr, &stateTexture[i]), "Ocean state");
            Check(device->CreateShaderResourceView(stateTexture[i].Get(), nullptr, &stateSRV[i]), "Ocean state SRV");
            Check(device->CreateUnorderedAccessView(stateTexture[i].Get(), nullptr, &stateUAV[i]), "Ocean state UAV");
            context->ClearUnorderedAccessViewFloat(stateUAV[i].Get(), zero);
        }
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(SprayParticle) * particleCount;
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        buffer.StructureByteStride = sizeof(SprayParticle);
        std::vector<SprayParticle> initial(particleCount);
        D3D11_SUBRESOURCE_DATA data{initial.data(), 0, 0};
        Check(device->CreateBuffer(&buffer, &data, &particlesBuffer), "Spray particles");
        Check(device->CreateShaderResourceView(particlesBuffer.Get(), nullptr, &particlesSRV), "Spray SRV");
        Check(device->CreateUnorderedAccessView(particlesBuffer.Get(), nullptr, &particlesUAV), "Spray UAV");
        buffer.ByteWidth = sizeof(UINT) * simulationResolution * simulationResolution;
        buffer.StructureByteStride = sizeof(UINT);
        buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        std::vector<UINT> impacts(simulationResolution * simulationResolution, 0);
        data.pSysMem = impacts.data();
        Check(device->CreateBuffer(&buffer, &data, &impactBuffer), "Spray impact grid");
        Check(device->CreateUnorderedAccessView(impactBuffer.Get(), nullptr, &impactUAV), "Spray impact UAV");
    }

    void AdvanceSimulation(float seconds, FrameConstants& frame)
    {
        constexpr float step = 1.0f / 60;
        float delta = previousSeconds < 0 ? step : std::clamp(seconds - previousSeconds, 0.0f, 0.1f);
        previousSeconds = seconds; accumulator += delta;
        ID3D11Buffer* cb = constants.Get();
        context->CSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* sampler = linearSampler.Get();
        context->CSSetSamplers(0, 1, &sampler);
        ID3D11ShaderResourceView* noSRV[6]{};
        ID3D11UnorderedAccessView* noUAV[3]{};
        for (UINT steps = 0; accumulator + 0.000001f >= step && steps < 6; ++steps) {
            simulationTime += step; accumulator = std::max(0.0f, accumulator - step);
            frame.cameraTime.w = simulationTime;
            float angle = 0.4f + 0.38f * std::sin(simulationTime * 0.09f) + 0.2f * std::sin(simulationTime * 0.037f);
            frame.wind = XMFLOAT4(std::cos(angle), std::sin(angle), 1, 1);
            context->UpdateSubresource(constants.Get(), 0, nullptr, &frame, 0, 0);
            UINT next = 1 - stateIndex;
            ID3D11ShaderResourceView* previous = stateSRV[stateIndex].Get();
            ID3D11UnorderedAccessView* output = stateUAV[next].Get();
            ID3D11UnorderedAccessView* impacts = impactUAV.Get();
            context->CSSetUnorderedAccessViews(2, 1, &impacts, nullptr);
            context->CSSetShaderResources(3, 1, &previous);
            context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
            context->CSSetShader(oceanCS.Get(), nullptr, 0);
            context->Dispatch(simulationResolution / 8, simulationResolution / 8, 1);
            context->CSSetUnorderedAccessViews(0, 3, noUAV, nullptr);
            context->CSSetShaderResources(0, 6, noSRV);
            stateIndex = next;
            ID3D11ShaderResourceView* current = stateSRV[stateIndex].Get();
            output = particlesUAV.Get();
            context->CSSetUnorderedAccessViews(2, 1, &impacts, nullptr);
            context->CSSetShaderResources(2, 1, &current);
            context->CSSetUnorderedAccessViews(1, 1, &output, nullptr);
            context->CSSetShader(sprayCS.Get(), nullptr, 0);
            context->Dispatch(particleCount / 64, 1, 1);
            context->CSSetUnorderedAccessViews(0, 3, noUAV, nullptr);
            context->CSSetShaderResources(0, 6, noSRV);
        }
        context->CSSetShader(nullptr, nullptr, 0);
    }

    void Fullscreen(ID3D11PixelShader* ps)
    {
        context->IASetInputLayout(nullptr);
        context->VSSetShader(fullscreenVS.Get(), nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->Draw(3, 0);
    }

    void CaptureFrame()
    {
        ComPtr<ID3D11Texture2D> back, staging;
        Check(swapChain->GetBuffer(0, IID_PPV_ARGS(&back)), "Capture back buffer");
        D3D11_TEXTURE2D_DESC desc{};
        back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        Check(device->CreateTexture2D(&desc, nullptr, &staging), "Capture staging");
        context->CopyResource(staging.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Capture map");
        // Copy rows before invoking the encoder so the GPU resource is always unmapped.
        std::vector<unsigned char> pixels(size_t(width) * height * 4);
        for (UINT y = 0; y < height; ++y)
            memcpy(pixels.data() + size_t(y) * width * 4,
                static_cast<unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(width) * 4);
        context->Unmap(staging.Get(), 0);
        ComPtr<IWICImagingFactory> factory;
        Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Capture WIC");
        ComPtr<IWICStream> stream;
        Check(factory->CreateStream(&stream), "Capture stream");
        auto path = shaderPath.parent_path() / (storm ? L"ocean-storm.png" : L"ocean-sunny.png");
        if (recordPreview) {
            auto directory = shaderPath.parent_path() / L"ocean-preview-frames";
            std::filesystem::create_directories(directory);
            auto number = std::to_wstring(previewFrame++);
            path = directory / (L"frame-" + std::wstring(4 - number.size(), L'0') + number + L".png");
        }
        Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Capture file");
        ComPtr<IWICBitmapEncoder> encoder;
        Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Capture encoder");
        Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Capture encoder init");
        ComPtr<IWICBitmapFrameEncode> frame;
        Check(encoder->CreateNewFrame(&frame, nullptr), "Capture frame");
        Check(frame->Initialize(nullptr), "Capture frame init");
        Check(frame->SetSize(width, height), "Capture size");
        for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        Check(frame->SetPixelFormat(&format), "Capture format");
        if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) throw std::runtime_error("Capture encoder changed pixel format");
        Check(frame->WritePixels(height, width * 4, UINT(pixels.size()), pixels.data()), "Capture pixels");
        Check(frame->Commit(), "Capture frame commit");
        Check(encoder->Commit(), "Capture commit");
    }

    void CreateGeometry()
    {
        auto terrainHeight = [](float x, float z) {
            return -18.0f + 23.0f * std::exp(-((x - 22) * (x - 22) + (z + 12) * (z + 12)) / 290.0f)
                + 0.20f * std::sin(x * 0.4f) * std::cos(z * 0.35f);
        };
        auto grid = [&](bool isWater, ComPtr<ID3D11Buffer>& vb, ComPtr<ID3D11Buffer>& ib, UINT& count) {
            const UINT resolution = isWater ? 512 : 256;
            const float extent = isWater ? 256.0f : 100.0f;
            std::vector<Vertex> vertices;
            std::vector<UINT> indices;
            for (UINT z = 0; z <= resolution; ++z) for (UINT x = 0; x <= resolution; ++x) {
                float u = -1 + 2.0f * x / resolution, v = -1 + 2.0f * z / resolution;
                // Concentrate water vertices near the orbit area, keeping the same distant extent.
                float px = extent * (isWater ? u * std::abs(u) : u);
                float pz = extent * (isWater ? v * std::abs(v) : v);
                XMFLOAT3 normal(0, 1, 0);
                if (!isWater) {
                    constexpr float e = 0.01f;
                    XMStoreFloat3(&normal, XMVector3Normalize(XMVectorSet(
                        -(terrainHeight(px + e, pz) - terrainHeight(px - e, pz)) / (2 * e), 1,
                        -(terrainHeight(px, pz + e) - terrainHeight(px, pz - e)) / (2 * e), 0)));
                }
                vertices.push_back({XMFLOAT3(px, isWater ? 0 : terrainHeight(px, pz), pz), normal});
            }
            for (UINT z = 0; z < resolution; ++z) for (UINT x = 0; x < resolution; ++x) {
                UINT a = z * (resolution + 1) + x, b = a + resolution + 1;
                indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
            D3D11_BUFFER_DESC desc{};
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.ByteWidth = UINT(vertices.size() * sizeof(Vertex)); desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA data{vertices.data(), 0, 0};
            Check(device->CreateBuffer(&desc, &data, &vb), "Grid vertices");
            desc.ByteWidth = UINT(indices.size() * sizeof(UINT)); desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            data.pSysMem = indices.data();
            Check(device->CreateBuffer(&desc, &data, &ib), "Grid indices");
            count = UINT(indices.size());
        };
        grid(false, terrainVB, terrainIB, terrainIndexCount);
        grid(true, waterVB, waterIB, waterIndexCount);
    }

    void Draw(ID3D11Buffer* vb, ID3D11Buffer* ib, UINT count, ID3D11VertexShader* vs, ID3D11PixelShader* ps)
    {
        UINT stride = sizeof(Vertex), offset = 0;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
        context->VSSetShader(vs, nullptr, 0); context->PSSetShader(ps, nullptr, 0);
        context->DrawIndexed(count, 0, 0);
    }
};

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto renderer = reinterpret_cast<Renderer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    static POINT lastMouse{};
    if (message == WM_NCCREATE) {
        renderer = static_cast<Renderer*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer));
    }
    try {
        switch (message) {
        case WM_SIZE:
            if (renderer) {
                renderer->minimized = wParam == SIZE_MINIMIZED;
                renderer->Resize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        case WM_LBUTTONDOWN:
            GetCursorPos(&lastMouse); SetCapture(window); return 0;
        case WM_LBUTTONUP:
            ReleaseCapture(); return 0;
        case WM_MOUSEMOVE:
            if (renderer && GetCapture() == window) {
                POINT mouse; GetCursorPos(&mouse);
                renderer->yaw -= (mouse.x - lastMouse.x) * 0.006f;
                renderer->pitch = std::clamp(renderer->pitch + (mouse.y - lastMouse.y) * 0.006f, 0.08f, 1.45f);
                lastMouse = mouse;
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (renderer) renderer->distance = std::clamp(renderer->distance - short(HIWORD(wParam)) / 120.0f * 2, 12.0f, 120.0f);
            return 0;
        case WM_KEYDOWN:
            if (renderer && wParam == '1') renderer->storm = false;
            if (renderer && wParam == '2') renderer->storm = true;
            if (renderer && wParam == '3' && !(lParam & (1LL << 30))) renderer->showFoam = !renderer->showFoam;
            if (renderer && wParam == '4' && !(lParam & (1LL << 30))) renderer->showSpray = !renderer->showSpray;
            if (wParam == VK_ESCAPE) DestroyWindow(window);
            if (wParam == VK_F5 && renderer && !(lParam & (1LL << 30))) {
                try { renderer->ReloadShaders(); }
                catch (const std::exception& error) { std::cerr << "Reload kept previous shaders: " << error.what() << '\n'; }
            }
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; PostQuitMessage(1); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int main(int argc, char** argv)
{
    const bool simulationTest = argc > 1 && std::string(argv[1]) == "--simulation-test";
    const bool record = argc > 1 && std::string(argv[1]) == "--record-preview";
    const bool smokeTest = record || simulationTest || (argc > 1 && std::string(argv[1]) == "--smoke-test");
    try {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM");
        struct COMScope { ~COMScope() { CoUninitialize(); } } comScope;
        Renderer renderer;
        renderer.recordPreview = record;
        for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--storm") renderer.storm = true;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc; wc.hInstance = instance;
        wc.lpszClassName = L"WaterPipelineWindow"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc)) throw std::runtime_error("RegisterClass failed");
        RECT rect{0, 0, LONG(renderer.width), LONG(renderer.height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        renderer.window = CreateWindowExW(0, wc.lpszClassName,
            L"Dynamic ocean | 1: sunny | 2: storm | 3: foam | 4: spray | F5: reload",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
            rect.bottom - rect.top, nullptr, nullptr, instance, &renderer);
        if (!renderer.window) throw std::runtime_error("CreateWindow failed");
        renderer.Initialize();
        if (simulationTest) {
            renderer.RunSimulationTest();
            DestroyWindow(renderer.window);
            return 0;
        }
        if (!smokeTest) ShowWindow(renderer.window, SW_SHOW);
        std::cout << "1: sunny, 2: storm, 3: toggle foam, 4: toggle spray, drag: orbit, wheel: zoom, F5: reload, Esc: exit.\n";
        const auto start = std::chrono::steady_clock::now();
        MSG message{};
        UINT renderedFrames = 0;
        while (message.message != WM_QUIT) {
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            } else if (renderer.minimized) WaitMessage();
            else {
                const bool capture = record ? renderedFrames >= 180 && renderedFrames % 6 == 0 : smokeTest && renderedFrames == 179;
                renderer.Render(smokeTest ? renderedFrames / 60.0f : std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count(), capture);
                if (smokeTest) {
                    ++renderedFrames;
                    if (renderedFrames == 2) renderer.Resize(960, 540);
                    if (renderedFrames == 3) renderer.ReloadShaders();
                    if (renderedFrames == (record ? 660u : 180u)) {
                        renderer.ValidateSimulation();
                        renderer.ValidateDebugMessages();
                        DestroyWindow(renderer.window);
                    }
                }
            }
        }
        return int(message.wParam);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (!smokeTest) MessageBoxA(nullptr, error.what(), "Water pipeline error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
