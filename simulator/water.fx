// Independent Caribbean-ocean study. All lighting is linear until TonePS.
cbuffer Frame : register(b0)
{
    row_major float4x4 ViewProjection;
    row_major float4x4 View;
    float4 CameraTime;
    float4 ViewportNearFar;
    float4 Water; // height, amplitude scale, frequency scale, speed scale
    float4 Ocean; // roughness, storm blend, exposure, whitecap strength
    float4 Simulation; // dt, span, resolution, wind strength
    float4 Wind; // direction x/z, gust, visible foam
};
Texture2D<float4> SceneColor : register(t0);
Texture2D<float> SceneDepth : register(t1);
SamplerState LinearClamp : register(s0);
Texture2D<float4> OceanState : register(t2);
Texture2D<float4> PreviousState : register(t3);
struct Particle { float4 positionAge; float4 velocityLife; };
StructuredBuffer<Particle> SprayParticles : register(t4);
Texture2D<float> WaterDepth : register(t5);
RWTexture2D<float4> NextState : register(u0);
RWStructuredBuffer<Particle> UpdatedParticles : register(u1);
RWStructuredBuffer<uint> SprayImpacts : register(u2);
static const float PI = 3.14159265;
struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; };
struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float crest : TEXCOORD2;
    float2 base : TEXCOORD3;
};
float Hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float Noise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x),
        lerp(Hash(i + float2(0, 1)), Hash(i + 1), f.x), f.y);
}
float FBM(float2 p)
{
    float result = 0, weight = 0.5;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        result += Noise(p) * weight;
        p = float2(p.x * 1.6 - p.y * 1.2, p.x * 1.2 + p.y * 1.6) + 7.1;
        weight *= 0.5;
    }
    return result;
}
float3 SunDirection() { return normalize(float3(-0.35, 0.48, -0.65)); }
float3 Atmosphere(float3 ray)
{
    float elevation = saturate(ray.y);
    float3 horizon = lerp(float3(0.62, 0.77, 0.86), float3(0.27, 0.32, 0.36), Ocean.y);
    float3 zenith = lerp(float3(0.10, 0.32, 0.63), float3(0.055, 0.085, 0.13), Ocean.y);
    float3 sky = lerp(horizon, zenith, pow(elevation, 0.55));
    float2 cloudUV = ray.xz / max(ray.y + 0.18, 0.12) * 1.8 + CameraTime.w * float2(0.012, 0.005);
    float cloud = smoothstep(lerp(0.53, 0.33, Ocean.y), 0.75, FBM(cloudUV));
    cloud *= smoothstep(0.015, 0.20, ray.y);
    sky = lerp(sky, lerp(float3(1.05, 1.08, 1.1), float3(0.31, 0.34, 0.38), Ocean.y), cloud * 0.85);
    float sun = saturate(dot(ray, SunDirection()));
    sky += (pow(sun, 512) * 8 + pow(sun, 24) * 0.18) * float3(1, 0.86, 0.66) * (1 - Ocean.y * 0.95);
    return sky;
}
float3 ApplyHaze(float3 color, float3 world)
{
    float3 ray = normalize(world - CameraTime.xyz);
    float distanceToCamera = length(world - CameraTime.xyz);
    // Fade the finite mesh into the same atmosphere used for the background.
    float haze = 1 - exp(-distanceToCamera * lerp(0.003, 0.008, Ocean.y));
    haze = max(haze, smoothstep(130, 230, distanceToCamera));
    return lerp(color, Atmosphere(ray), haze);
}
float4 FullscreenVS(uint id : SV_VertexID) : SV_POSITION
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 1, 1);
}
float4 SkyPS(float4 position : SV_POSITION) : SV_TARGET
{
    float2 ndc = position.xy / ViewportNearFar.xy * float2(2, -2) + float2(-1, 1);
    float3 forward = normalize(-CameraTime.xyz);
    float3 right = normalize(cross(float3(0, 1, 0), forward));
    float3 up = cross(forward, right);
    float3 ray = normalize(forward + right * ndc.x * (ViewportNearFar.x / ViewportNearFar.y) * 0.41421356
        + up * ndc.y * 0.41421356);
    return float4(Atmosphere(ray), 1);
}
VertexOutput SceneVS(VertexInput input)
{
    VertexOutput output;
    output.world = input.position;
    output.normal = input.normal;
    output.crest = 0;
    output.base = input.position.xz;
    output.position = mul(float4(output.world, 1), ViewProjection);
    return output;
}
float4 ScenePS(VertexOutput input) : SV_TARGET
{
    float variation = FBM(input.world.xz * 0.22);
    float3 sand = float3(0.56, 0.43, 0.23) * (0.75 + variation * 0.5);
    float3 grass = float3(0.075, 0.19, 0.035);
    float3 albedo = lerp(sand, grass, smoothstep(1.0, 3.2, input.world.y));
    float diffuse = saturate(dot(normalize(input.normal), SunDirection()));
    float3 color = albedo * (0.28 + diffuse * lerp(1.25, 0.45, Ocean.y));
    return float4(ApplyHaze(color, input.world), 1);
}
// Traveling-wave interference and breaking approximation, not a full 3D fluid solver.
float2 VirtualWind(float time)
{
    float angle = 0.4 + 0.38 * sin(time * 0.09) + 0.2 * sin(time * 0.037);
    return float2(cos(angle), sin(angle));
}
float TerrainHeight(float2 p)
{
    float2 q = p - float2(22, -12);
    return -18 + 23 * exp(-dot(q, q) / 290) + 0.2 * sin(p.x * 0.4) * cos(p.y * 0.35);
}
float2 StateUV(float2 p) { return p / Simulation.y + 0.5; }
float DomainFade(float2 p)
{
    return 1 - smoothstep(Simulation.y * 0.40, Simulation.y * 0.49, max(abs(p.x), abs(p.y)));
}
struct Surface
{
    float3 position;
    float3 dx;
    float3 dz;
    float3 velocity;
    float compression;
    float crossing;
    float2 momentum;
};
void WaveComponent(float2 base, float2 direction, float wavelength, float amplitude,
    float steepness, float phaseOffset, float envelope, float2 envelopeGradient, inout Surface surface)
{
    float k = 2 * PI / wavelength * Water.z;
    float omega = sqrt(9.81 * k) * Water.w;
    float phase = k * dot(direction, base) - omega * CameraTime.w + phaseOffset;
    float a = amplitude * Water.y;
    float horizontal = steepness / k;
    float sn = sin(phase), cs = cos(phase);
    float3 offset = float3(direction.x * horizontal * cs, a * sn, direction.y * horizontal * cs);
    surface.position += offset * envelope;
    surface.dx += float3(-steepness * direction.x * direction.x * sn,
        a * k * direction.x * cs, -steepness * direction.x * direction.y * sn) * envelope + offset * envelopeGradient.x;
    surface.dz += float3(-steepness * direction.x * direction.y * sn,
        a * k * direction.y * cs, -steepness * direction.y * direction.y * sn) * envelope + offset * envelopeGradient.y;
    surface.velocity += float3(direction.x * horizontal * omega * sn,
        -a * omega * cs, direction.y * horizontal * omega * sn) * envelope;
    float crest = max(sn, 0) * steepness * envelope;
    surface.compression += crest;
    surface.momentum += direction * crest;
}
Surface EvaluateSurface(float2 base)
{
    Surface surface;
    surface.position = float3(base.x, Water.x, base.y);
    surface.dx = float3(1, 0, 0); surface.dz = float3(0, 0, 1);
    surface.velocity = 0; surface.compression = 0; surface.crossing = 0; surface.momentum = 0;
    WaveComponent(base, normalize(float2(1, 0.35)), 31, 0.48, 0.13, 0, 1, 0, surface);
    WaveComponent(base, normalize(float2(-0.35, 1)), 22, 0.28, 0.10, 1.7, 1, 0, surface);
    WaveComponent(base, normalize(float2(-0.8, -0.6)), 14, 0.15, 0.07, 3.1, 1, 0, surface);
    // New packets remember wind at birth; two generations overlap without phase resets.
    [loop] for (int lane = 0; lane < 6; ++lane)
    {
        float generation = floor((CameraTime.w - lane * 3.1) / 18);
        [unroll] for (int previous = 0; previous < 2; ++previous)
        {
            float epoch = generation - previous;
            float birth = epoch * 18 + lane * 3.1;
            float age = CameraTime.w - birth;
            float2 seed = float2(lane * 17.31, epoch * 11.73);
            float r = Hash(seed + 3.7);
            float2 oldWind = VirtualWind(birth);
            float spread = (r - 0.5) * 1.7 + ((lane == 0 || lane == 3) ? 2.0 : 0);
            float2 dir = float2(oldWind.x * cos(spread) - oldWind.y * sin(spread),
                oldWind.x * sin(spread) + oldWind.y * cos(spread));
            float wavelength = 7 + Hash(seed + 9.2) * 17;
            float k = 2 * PI / wavelength * Water.z;
            float2 center = (float2(Hash(seed + 2.1), Hash(seed + 7.8)) - 0.5) * 120;
            center += dir * sqrt(9.81 / k) * 0.5 * Water.w * (age - 10);
            float2 q = base - center;
            float2 side = float2(-dir.y, dir.x);
            float along = dot(q, dir), across = dot(q, side);
            float width = 18 + r * 22;
            float spatial = exp(-0.5 * (along * along / (width * width) + across * across / 1600));
            float temporal = smoothstep(0, 3.5, age) * (1 - smoothstep(18, 29, age));
            float gust = 0.8 + 0.3 * sin(birth * 0.13 + r * 6);
            float envelope = spatial * temporal * gust;
            float2 gradient = -envelope * (dir * along / (width * width) + side * across / 1600);
            WaveComponent(base, dir, wavelength, 0.22 + r * 0.25,
                0.12 + r * 0.06, Hash(seed + 4.6) * 2 * PI, envelope, gradient, surface);
        }
    }
    float jacobian = surface.dx.x * surface.dz.z - surface.dx.z * surface.dz.x;
    surface.crossing = max(surface.compression - length(surface.momentum), 0);
    float squeeze = 1 - smoothstep(0.42, 0.90, jacobian);
    surface.compression = saturate(squeeze * 0.7 + surface.crossing * 2.8);
    return surface;
}
VertexOutput WaterVS(VertexInput input)
{
    VertexOutput output;
    Surface surface = EvaluateSurface(input.position.xz);
    float2 uv = StateUV(input.position.xz);
    float fade = DomainFade(input.position.xz);
    float cell = Simulation.y / Simulation.z;
    float4 state = OceanState.SampleLevel(LinearClamp, uv, 0);
    float hx = (OceanState.SampleLevel(LinearClamp, uv + float2(1 / Simulation.z, 0), 0).x
        - OceanState.SampleLevel(LinearClamp, uv - float2(1 / Simulation.z, 0), 0).x) / (2 * cell);
    float hz = (OceanState.SampleLevel(LinearClamp, uv + float2(0, 1 / Simulation.z), 0).x
        - OceanState.SampleLevel(LinearClamp, uv - float2(0, 1 / Simulation.z), 0).x) / (2 * cell);
    surface.position.y += state.x * fade;
    surface.dx.y += hx * fade; surface.dz.y += hz * fade;
    output.world = surface.position;
    output.normal = normalize(cross(surface.dz, surface.dx));
    output.crest = surface.compression;
    output.base = input.position.xz;
    output.position = mul(float4(output.world, 1), ViewProjection);
    return output;
}
float LinearDepth(float depth)
{
    float n = ViewportNearFar.z, f = ViewportNearFar.w;
    return n * f / (f - depth * (f - n));
}
float2 RippleSlope(float2 p)
{
    float t = CameraTime.w * Water.w;
    float2 slope = 0;
    // Pixel-only ripples: fade frequencies that become smaller than a pixel.
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float angle = atan2(Wind.y, Wind.x) + i * 1.27;
        float2 d = float2(cos(angle), sin(angle));
        float k = 3.5 + i * 2.6;
        float phase = dot(p, d) * k - t * (2.1 + i * 0.65);
        float filter = 1 - smoothstep(0.6, 2.2, fwidth(phase));
        slope += d * cos(phase) * (0.085 / (1 + i * 0.45)) * filter;
    }
    return slope;
}
float SunSpecular(float3 n, float3 v, float roughness)
{
    float3 l = SunDirection(), h = normalize(v + l);
    float nv = max(saturate(dot(n, v)), 0.02), nl = saturate(dot(n, l));
    float nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    float a = roughness * roughness, a2 = a * a;
    float denominator = nh * nh * (a2 - 1) + 1;
    float distribution = a2 / max(PI * denominator * denominator, 0.000001);
    float k = (roughness + 1) * (roughness + 1) / 8;
    float geometry = nv / (nv * (1 - k) + k) * nl / max(nl * (1 - k) + k, 0.0001);
    float fresnel = 0.0204 + 0.9796 * pow(1 - vh, 5);
    return distribution * geometry * fresnel / max(4 * nv * nl, 0.0001) * nl;
}
float4 WaterPS(VertexOutput input) : SV_TARGET
{
    float2 uv = input.position.xy / ViewportNearFar.xy;
    float2 ripple = RippleSlope(input.world.xz);
    float3 normal = normalize(input.normal + float3(-ripple.x, 0, -ripple.y));
    float3 view = normalize(CameraTime.xyz - input.world);
    float surfaceDepth = LinearDepth(input.position.z);
    float backgroundDepth = LinearDepth(SceneDepth.Load(int3(int2(input.position.xy), 0)));
    // Convert view-Z separation to approximate distance along the view ray.
    float3 viewRay = mul(float4(-view, 0), View).xyz;
    float thickness = max(backgroundDepth - surfaceDepth, 0) / max(abs(viewRay.z), 0.15);
    float3 viewNormal = mul(float4(normal, 0), View).xyz;
    float2 refractedUV = saturate(uv + viewNormal.xy * float2(1, -1) * 0.014 * saturate(thickness * 0.3));
    int2 pixel = clamp(int2(refractedUV * ViewportNearFar.xy), int2(0, 0), int2(ViewportNearFar.xy) - 1);
    if (LinearDepth(SceneDepth.Load(int3(pixel, 0))) <= surfaceDepth) refractedUV = uv;
    float3 background = SceneColor.Sample(LinearClamp, refractedUV).rgb;
    float3 absorption = float3(0.22, 0.065, 0.035);
    float3 transmission = exp(-absorption * thickness);
    float3 deepColor = lerp(float3(0.008, 0.095, 0.14), float3(0.009, 0.032, 0.043), Ocean.y);
    float3 underwater = background * transmission + deepColor * (1 - transmission);
    float fresnel = 0.0204 + 0.9796 * pow(1 - saturate(dot(normal, view)), 5);
    float3 reflection = Atmosphere(reflect(-view, normal));
    float3 color = lerp(underwater, reflection, fresnel);
    color += SunSpecular(normal, view, Ocean.x) * float3(7, 6.4, 5.2) * (1 - Ocean.y * 0.95);
    // Lift sun-facing wave slopes with a restrained turquoise subsurface approximation.
    float backlight = pow(saturate(dot(view, -SunDirection())), 3);
    color += float3(0.012, 0.12, 0.10) * backlight * saturate(input.world.y - Water.x) * (1 - fresnel);
    float foamNoise = FBM(input.world.xz * 1.2 + CameraTime.w * float2(-0.12, 0.07));
    float persistentFoam = OceanState.Sample(LinearClamp, StateUV(input.base)).z * DomainFade(input.base);
    float crestFoam = saturate(persistentFoam * 1.1 + input.crest * 0.025) * smoothstep(0.20, 0.65, foamNoise);
    float shoreFoam = (1 - smoothstep(0.10, 0.85, thickness)) * smoothstep(0.22, 0.65, foamNoise);
    float foam = saturate(crestFoam + shoreFoam * 0.75) * Wind.w;
    color = lerp(color, lerp(float3(0.85, 0.95, 0.92), float3(0.38, 0.44, 0.46), Ocean.y), foam);
    return float4(ApplyHaze(color, input.world), 1);
}
float3 LinearToSRGB(float3 color)
{
    float3 low = color * 12.92;
    float3 high = 1.055 * pow(max(color, 0), 1.0 / 2.4) - 0.055;
    return float3(color.r <= 0.0031308 ? low.r : high.r,
        color.g <= 0.0031308 ? low.g : high.g, color.b <= 0.0031308 ? low.b : high.b);
}
float4 TonePS(float4 position : SV_POSITION) : SV_TARGET
{
    float3 hdr = max(SceneColor.Load(int3(int2(position.xy), 0)).rgb * Ocean.z, 0);
    // ACES-inspired fitted curve; highlights retain shape instead of clipping in UNORM.
    float3 mapped = saturate((hdr * (2.51 * hdr + 0.03)) / (hdr * (2.43 * hdr + 0.59) + 0.14));
    return float4(LinearToSRGB(mapped), 1);
}

// Persistent local disturbance/foam field: h, dh/dt, foam coverage, breaking energy.
[numthreads(8, 8, 1)]
void OceanCS(uint3 id : SV_DispatchThreadID)
{
    int2 pixel = int2(id.xy), limit = int2(Simulation.z, Simulation.z) - 1;
    float cell = Simulation.y / Simulation.z, dt = Simulation.x;
    float2 uv = (float2(pixel) + 0.5) / Simulation.z;
    float2 base = (uv - 0.5) * Simulation.y;
    float4 old = PreviousState.Load(int3(pixel, 0));
    uint impacts;
    InterlockedExchange(SprayImpacts[id.y * uint(Simulation.z) + id.x], 0, impacts);
    float impactEnergy = min(float(impacts), 16);
    float left = PreviousState.Load(int3(clamp(pixel + int2(-1, 0), 0, limit), 0)).x;
    float right = PreviousState.Load(int3(clamp(pixel + int2(1, 0), 0, limit), 0)).x;
    float down = PreviousState.Load(int3(clamp(pixel + int2(0, -1), 0, limit), 0)).x;
    float up = PreviousState.Load(int3(clamp(pixel + int2(0, 1), 0, limit), 0)).x;
    Surface surface = EvaluateSurface(base);
    float wet = smoothstep(0.05, 0.5, surface.position.y - TerrainHeight(base));
    float rising = saturate(0.15 + surface.velocity.y * 0.40);
    float breaker = smoothstep(0.38, 0.78, surface.compression) * rising * wet;
    float shore = (1 - smoothstep(0.15, 1.5, surface.position.y - TerrainHeight(base))) * wet;
    breaker = saturate(breaker + shore * saturate(surface.velocity.y) * 0.6);
    // Crest convergence transfers energy into local waves. These disturbances
    // propagate through neighbors instead of being recomputed as a sine pattern.
    float impulse = (breaker - old.w) * 0.9 - impactEnergy * 0.015;
    float gust = Noise(base * 0.08 - Wind.xy * CameraTime.w * 0.10) - 0.5;
    float laplacian = (left + right + down + up - 4 * old.x) / (cell * cell);
    float speed2 = 16 * (1 + min(abs(old.x), 0.8) * 0.4);
    float velocity = old.y + dt * (speed2 * laplacian - old.x * 1.3 + gust * 0.16 * Simulation.w);
    velocity = clamp((velocity + impulse) * exp(-dt * 0.75), -5, 5);
    float height = clamp(old.x + velocity * dt, -1.5, 1.5);
    float2 flow = surface.velocity.xz * 0.23 + Wind.xy * 0.65 * Simulation.w;
    float foam = PreviousState.SampleLevel(LinearClamp, uv - flow * dt / Simulation.y, 0).z;
    foam = foam * exp(-dt * 0.35) + dt * (breaker * 0.8 + shore * 0.35) * Ocean.w + impactEnergy * 0.015;
    float edge = DomainFade(base);
    float damping = exp(-dt * (1 - edge) * 15);
    NextState[pixel] = float4(height * damping * wet, velocity * damping * wet,
        saturate(foam) * damping * wet, breaker * wet * edge);
}
[numthreads(64, 1, 1)]
void SprayCS(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    Particle p = UpdatedParticles[index];
    float dt = Simulation.x;
    float tick = floor(CameraTime.w / dt + 0.5);
    float2 seed = float2(index * 1.319, tick * 0.713);
    if (p.positionAge.w < p.velocityLife.w)
    {
        p.positionAge.w += dt;
        p.velocityLife.xyz += float3(Wind.x * Simulation.w * 0.7, -9.81, Wind.y * Simulation.w * 0.7) * dt;
        p.velocityLife.xyz *= exp(-dt * 0.32);
        p.positionAge.xyz += p.velocityLife.xyz * dt;
        // Returning spray deposits foam and a small disturbance for the next simulation step.
        Surface surface = EvaluateSurface(p.positionAge.xz);
        float h = surface.position.y + OceanState.SampleLevel(LinearClamp, StateUV(p.positionAge.xz), 0).x;
        if (p.velocityLife.y < 0 && p.positionAge.y < h) {
            float2 uv = StateUV(p.positionAge.xz);
            if (all(uv > 0) && all(uv < 1) && p.positionAge.y > TerrainHeight(p.positionAge.xz)) {
                uint2 hit = uint2(uv * Simulation.z);
                InterlockedAdd(SprayImpacts[hit.y * uint(Simulation.z) + hit.x], 1);
            }
            p.positionAge.w = p.velocityLife.w;
        }
        if (p.positionAge.y < TerrainHeight(p.positionAge.xz)) p.positionAge.w = p.velocityLife.w;
    }
    else
    {
        // Deterministic stochastic emission; probability is per-second, not per-frame.
        float2 base = (float2(Hash(seed + 4.7), Hash(seed + 8.2)) - 0.5) * 140;
        float4 state = OceanState.SampleLevel(LinearClamp, StateUV(base), 0);
        float breaking = state.w;
        if (Hash(seed + 2.5) < dt * 9 * breaking)
        {
            Surface surface = EvaluateSurface(base);
            float3 position = surface.position + float3(0, state.x + 0.10, 0);
            if (position.y > TerrainHeight(position.xz) + 0.20)
            {
                float3 velocity = surface.velocity * 0.6;
                velocity += float3(Wind.x, 0, Wind.y) * (0.7 + Hash(seed + 6.1) * 1.8);
                velocity.xz += (float2(Hash(seed + 3.9), Hash(seed + 7.1)) - 0.5) * (2 + breaking * 3);
                velocity.y = 2 + breaking * 3.5 + Hash(seed + 1.1) * 2;
                p.positionAge = float4(position, 0);
                p.velocityLife = float4(velocity, 0.7 + Hash(seed + 5.1) * 1.1);
            }
        }
    }
    UpdatedParticles[index] = p;
}
struct SprayOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float opacity : TEXCOORD1;
    float3 world : TEXCOORD2;
};
SprayOutput SprayVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1, -1), float2(-1, 1), float2(1, -1),
        float2(1, -1), float2(-1, 1), float2(1, 1)};
    Particle p = SprayParticles[instance];
    SprayOutput output;
    float life = max(p.velocityLife.w, 0.001);
    float age = saturate(p.positionAge.w / life);
    float size = lerp(0.055, 0.13, Hash(float2(instance, 1))) * (1 + age * 0.7);
    float3 right = float3(View[0][0], View[1][0], View[2][0]);
    float3 up = float3(View[0][1], View[1][1], View[2][1]);
    float2 corner = corners[vertex];
    float3 projectedVelocity = mul(float4(p.velocityLife.xyz, 0), View).xyz;
    float2 streak = projectedVelocity.xy * 0.012 * corner.y;
    output.world = p.positionAge.xyz + right * (corner.x * size + streak.x) + up * (corner.y * size + streak.y);
    output.position = mul(float4(output.world, 1), ViewProjection);
    output.uv = corner;
    output.opacity = smoothstep(0, 0.08, age) * (1 - smoothstep(0.55, 1, age)) * 0.58;
    if (p.positionAge.w >= p.velocityLife.w || p.velocityLife.w <= 0) {
        output.position = float4(2, 2, 2, 1); output.opacity = 0;
    }
    return output;
}
float4 SprayPS(SprayOutput input) : SV_TARGET
{
    float r2 = dot(input.uv, input.uv);
    float alpha = exp(-r2 * 2.8) * (1 - smoothstep(0.7, 1, r2)) * input.opacity;
    float opaqueDepth = LinearDepth(WaterDepth.Load(int3(int2(input.position.xy), 0)));
    float particleDepth = LinearDepth(input.position.z);
    alpha *= saturate((opaqueDepth - particleDepth) / 0.35);
    float3 color = lerp(float3(1.35, 1.45, 1.5), float3(0.65, 0.72, 0.78), Ocean.y);
    color = ApplyHaze(color, input.world);
    return float4(color * alpha, alpha); // Premultiplied alpha; unsorted, near-identical-tint sprite approximation.
}
