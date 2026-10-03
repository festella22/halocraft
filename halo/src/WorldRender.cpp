#define NOMINMAX
#include "WorldRender.hpp"
#include <Windows.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>
#include "Coords.hpp"
#include "Dig.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "WorldEntities.hpp"

namespace WorldRender {
    namespace {
        namespace proto = skycraft::proto;

        constexpr std::uint64_t kDrainBytesPerFrame = 48ull << 20;  // the 21 MB atlas must fit in one go
        // Halo's level shader keeps its view-projection in the first 4 rows of vertex constant
        // buffer 0 (row-major, Halo world space, reversed infinite depth: depth = near / view z).
        constexpr UINT kViewProjBytes = 64;

        struct Section {
            ID3D11Buffer* vb = nullptr;
            UINT count = 0;
            int sx, sy, sz;
        };

        struct alignas(16) FrameCB {
            float misc[4];  // blocks per unit, daylight, pass (0 solid, 1 translucent), -
        };
        struct alignas(16) SectionCB {
            float origin[4];  // section origin, Halo world units
        };

        std::unordered_map<std::uint64_t, Section> sections;
        ID3D11Texture2D* atlas = nullptr;
        ID3D11ShaderResourceView* atlasSrv = nullptr;
        UINT atlasW = 0, atlasH = 0;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11InputLayout* layout = nullptr;
        ID3D11Buffer* frameCb = nullptr;
        ID3D11Buffer* sectionCb = nullptr;
        ID3D11Buffer* viewProjCb = nullptr;  // filled GPU-side from Halo's own constants every frame
        ID3D11SamplerState* sampler = nullptr;
        ID3D11RasterizerState* raster = nullptr;
        ID3D11DepthStencilState* depthWrite = nullptr;
        ID3D11DepthStencilState* depthTest = nullptr;
        ID3D11BlendState* opaque = nullptr;
        ID3D11BlendState* alphaBlend = nullptr;
        ID3D11BlendState* crumble = nullptr;        // Minecraft's crack blend: 2 * src * dst
        ID3D11RasterizerState* biased = nullptr;    // cracks: pulled towards the camera so they never z-fight
        bool initFailed = false;

        // Halo's scene depth (R32G8X24_TYPELESS) and our depth view of it: blocks are tested and
        // written against it, so Halo's walls hide blocks and blocks hide each other.
        ID3D11Texture2D* haloDepth = nullptr;
        ID3D11DepthStencilView* haloDsv = nullptr;
        UINT haloDepthW = 0, haloDepthH = 0;
        bool sceneSeen = false;  // Halo drew its level (and we copied its matrix) since the last Present

        // Dug holes in Halo's ground: each dug block (merged into boxes) is drawn as a box; where the
        // Halo surface seen through it lies inside the box, that pixel becomes empty (depth reset to
        // the far plane) so the hole's Minecraft walls and blocks draw there. Halo's depth is read
        // from a copy (it's also the depth target).
        struct HoleVertex {
            float pos[3], lo[3], hi[3];  // Minecraft coordinates relative to holeOrigin
        };
        ID3D11VertexShader* holeVs = nullptr;
        ID3D11PixelShader* holePs = nullptr;
        ID3D11InputLayout* holeLayout = nullptr;
        ID3D11DepthStencilState* depthOverwrite = nullptr;
        ID3D11Texture2D* depthCopy = nullptr;
        ID3D11ShaderResourceView* depthCopySrv = nullptr;
        ID3D11Buffer* holeVb = nullptr;
        UINT holeCapacity = 0;
        std::vector<HoleVertex> holeVerts;
        double holeOrigin[3]{};
        bool holesDirty = true;
        constexpr int kHoleReach = 64, kHoleReachY = 32;  // blocks around the player whose holes are drawn

        // Minecraft's entity renderer output: the player's body in third person (relative to the
        // feet), and everything else (chests, beds, minecarts, TNT, particles) relative to an origin.
        struct Mesh {
            double origin[3]{};
            std::vector<proto::RenBatch> batches;
            std::vector<proto::RenVertex> verts;
            ID3D11Buffer* vb = nullptr;
            UINT capacity = 0;
            bool dirty = false;
        };
        Mesh avatar, scene;
        struct EntityTexture {
            ID3D11Texture2D* tex = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
        };
        std::unordered_map<std::uint32_t, EntityTexture> entityTextures;  // skins, armour, ... (RenTexture ids)

        // Per-frame things (dropped items, arrows, cracks, the outline), rebuilt every frame.
        proto::WorldEntities entities{};
        std::vector<proto::RenVertex> dynTris, dynCracks, dynLines;
        ID3D11Buffer* dynVb = nullptr;
        UINT dynCapacity = 0;

        constexpr char kShader[] = R"(
cbuffer Frame : register(b0) { float4 misc; };
cbuffer Section : register(b1) { float4 origin; };
cbuffer Halo : register(b2) { float4 vp0; float4 vp1; float4 vp2; float4 vp3; };
Texture2D atlas : register(t0);
SamplerState samp : register(s0);
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; uint light : TEXCOORD1; uint flags : TEXCOORD2; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; float2 light : TEXCOORD1; nointerpolation uint flags : TEXCOORD2; };
VSOut VSMain(VSIn i) {
	VSOut o;
	float4 world = float4(origin.xyz + float3(i.pos.x, -i.pos.z, i.pos.y) / misc.x, 1.0);  // Minecraft -> Halo axes
	o.pos = float4(dot(vp0, world), dot(vp1, world), dot(vp2, world), dot(vp3, world));
	o.uv = i.uv;
	o.color = i.color;
	o.light = float2(i.light & 0xFF, (i.light >> 8) & 0xFF) / 15.0;
	o.flags = i.flags;
	return o;
}
float Curve(float l) { return l / (4.0 - 3.0 * l); }  // Minecraft's light-level falloff
// Minecraft's fixed face shading by Direction ordinal + 1: none, down, up, north, south, west, east.
static const float kShade[8] = { 1.0, 0.5, 1.0, 0.8, 0.8, 0.6, 0.6, 1.0 };
float4 PSMain(VSOut i) : SV_Target {
	float4 tex = (i.flags & 0x100) != 0 ? float4(1, 1, 1, 1) : atlas.Sample(samp, i.uv);  // 0x100: untextured (outline)
	bool translucent = (i.flags & 2) != 0;
	if (translucent != (misc.z > 0.5)) discard;
	if ((i.flags & 1) != 0 && tex.a < 0.1) discard;  // cutout: leaves, glass panes, flowers
	float light = max(Curve(i.light.x), Curve(i.light.y) * misc.y);
	float3 rgb = tex.rgb * i.color.rgb * kShade[(i.flags >> 4) & 7] * lerp(0.08, 1.0, light);
	return float4(rgb, translucent ? tex.a * i.color.a : 1.0);
}
)";

        constexpr char kHoleShader[] = R"(
cbuffer Frame : register(b0) { float4 misc; };
cbuffer Section : register(b1) { float4 origin; };
cbuffer Halo : register(b2) { float4 vp0; float4 vp1; float4 vp2; float4 vp3; };
Texture2D<float> haloDepth : register(t1);
struct VSIn { float3 pos : POSITION; float3 lo : TEXCOORD0; float3 hi : TEXCOORD1; };
struct VSOut {
	float4 pos : SV_Position; float3 world : TEXCOORD0;
	nointerpolation float3 lo : TEXCOORD1; nointerpolation float3 hi : TEXCOORD2; nointerpolation float3 cam : TEXCOORD3;
};
VSOut VSMain(VSIn i) {
	VSOut o;
	o.world = origin.xyz + float3(i.pos.x, -i.pos.z, i.pos.y) / misc.x;
	float4 w = float4(o.world, 1.0);
	o.pos = float4(dot(vp0, w), dot(vp1, w), dot(vp2, w), dot(vp3, w));
	o.lo = i.lo;
	o.hi = i.hi;
	// The camera: the one point where the matrix's x, y and w rows are all zero.
	float3 r0 = vp0.xyz, r1 = vp1.xyz, r3 = vp3.xyz;
	o.cam = -(vp0.w * cross(r1, r3) + vp1.w * cross(r3, r0) + vp3.w * cross(r0, r1)) / dot(r0, cross(r1, r3));
	return o;
}
struct PSOut { float4 color : SV_Target; float depth : SV_Depth; };
PSOut PSMain(VSOut i) {
	float raw = haloDepth.Load(int3(i.pos.xy, 0));
	if (raw <= 0.0) discard;  // sky: nothing there
	// The Halo surface this pixel shows: on the camera ray through this point, at Halo's depth.
	float3 d = i.world - i.cam;
	float s = dot(vp2, float4(i.cam, 1.0)) / (raw * dot(vp3.xyz, d) - dot(vp2.xyz, d));
	float3 S = i.cam + d * s;
	float3 mc = float3(S.x - origin.x, S.z - origin.z, origin.y - S.y) * misc.x;  // Halo -> Minecraft, relative
	const float e = 0.005;
	if (any(mc < i.lo - e) || any(mc > i.hi + e)) discard;  // that surface isn't dug
	PSOut o;
	o.color = float4(0.03, 0.03, 0.03, 1.0);  // only shows if Minecraft has nothing there yet
	o.depth = 0.0;  // the far plane: whatever Minecraft has in the hole draws over it
	return o;
}
)";

        template <class T> void rel(T*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        }

        std::uint64_t key(int x, int y, int z) {
            return (std::uint64_t(x & 0x1FFFFF) << 42) | (std::uint64_t(y & 0x1FFFFF) << 21) | std::uint64_t(z & 0x1FFFFF);
        }

        bool init(ID3D11Device* device) {
            if (vs)
                return true;
            if (initFailed)
                return false;
            ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errors = nullptr;
            auto compile = [&](std::string_view source, const char* entry, const char* target, ID3DBlob** out) {
                const auto hr = D3DCompile(source.data(), source.size(), "halocraft_blocks", nullptr, nullptr, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
                if (FAILED(hr))
                    log(std::string("block shader ") + entry + " failed: " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?"));
                rel(errors);
                return SUCCEEDED(hr);
            };
            if (!compile(kHoleShader, "VSMain", "vs_5_0", &vsBlob) || !compile(kHoleShader, "PSMain", "ps_5_0", &psBlob)) {
                rel(vsBlob);
                initFailed = true;
                return false;
            }
            device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &holeVs);
            device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &holePs);
            const D3D11_INPUT_ELEMENT_DESC holeElements[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            device->CreateInputLayout(holeElements, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &holeLayout);
            rel(vsBlob);
            rel(psBlob);
            if (!compile(kShader, "VSMain", "vs_5_0", &vsBlob) || !compile(kShader, "PSMain", "ps_5_0", &psBlob)) {
                rel(vsBlob);
                initFailed = true;
                return false;
            }
            device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
            device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
            const D3D11_INPUT_ELEMENT_DESC elements[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            device->CreateInputLayout(elements, 5, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);
            rel(vsBlob);
            rel(psBlob);

            D3D11_BUFFER_DESC cbd{};
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            cbd.ByteWidth = sizeof(FrameCB);
            device->CreateBuffer(&cbd, nullptr, &frameCb);
            cbd.ByteWidth = sizeof(SectionCB);
            device->CreateBuffer(&cbd, nullptr, &sectionCb);
            cbd.Usage = D3D11_USAGE_DEFAULT;  // a copy destination, never touched by the CPU
            cbd.CPUAccessFlags = 0;
            cbd.ByteWidth = kViewProjBytes;
            device->CreateBuffer(&cbd, nullptr, &viewProjCb);

            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            device->CreateSamplerState(&sd, &sampler);

            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;  // ponytail: no back-face culling; Minecraft's winding isn't pinned down
            rd.DepthClipEnable = TRUE;
            device->CreateRasterizerState(&rd, &raster);

            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = TRUE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;  // reversed depth: nearer is larger
            device->CreateDepthStencilState(&dd, &depthWrite);
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            device->CreateDepthStencilState(&dd, &depthTest);
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            device->CreateDepthStencilState(&dd, &depthOverwrite);

            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            device->CreateBlendState(&bd, &opaque);
            auto& rt = bd.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
            rt.DestBlendAlpha = D3D11_BLEND_ONE;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            device->CreateBlendState(&bd, &alphaBlend);
            rt.SrcBlend = D3D11_BLEND_DEST_COLOR;
            rt.DestBlend = D3D11_BLEND_SRC_COLOR;
            device->CreateBlendState(&bd, &crumble);
            rd.SlopeScaledDepthBias = 1.0f;  // reversed depth: positive is nearer
            rd.DepthBias = 16;
            device->CreateRasterizerState(&rd, &biased);

            const bool ok = vs && ps && layout && frameCb && sectionCb && viewProjCb && sampler && raster && depthWrite && depthTest && opaque &&
                            alphaBlend && crumble && biased && holeVs && holePs && holeLayout && depthOverwrite;
            log(ok ? "block renderer ready" : "block renderer failed to initialize");
            initFailed = !ok;
            return ok;
        }

        void dropSections() {
            for (auto& [k, s] : sections)
                rel(s.vb);
            sections.clear();
        }

        void onAtlas(ID3D11Device* device, const std::uint8_t* p, std::uint32_t bytes) {
            proto::RenAtlas hdr{};
            std::memcpy(&hdr, p, sizeof(hdr));
            if (bytes < sizeof(hdr) + std::uint64_t(hdr.width) * hdr.height * 4)
                return;
            rel(atlasSrv);
            rel(atlas);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = hdr.width;
            td.Height = hdr.height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{ p + sizeof(hdr), hdr.width * 4, 0 };
            if (FAILED(device->CreateTexture2D(&td, &init, &atlas)) || FAILED(device->CreateShaderResourceView(atlas, nullptr, &atlasSrv))) {
                log("block atlas creation failed");
                return;
            }
            atlasW = hdr.width;
            atlasH = hdr.height;
            log("block atlas " + std::to_string(atlasW) + "x" + std::to_string(atlasH));
        }

        void onAtlasRegion(ID3D11DeviceContext* context, const std::uint8_t* p, std::uint32_t bytes) {
            proto::RenAtlasRegion r{};
            std::memcpy(&r, p, sizeof(r));
            if (!atlas || r.x + r.width > atlasW || r.y + r.height > atlasH || bytes < sizeof(r) + std::uint64_t(r.width) * r.height * 4)
                return;
            const D3D11_BOX box{ r.x, r.y, 0, r.x + r.width, r.y + r.height, 1 };
            context->UpdateSubresource(atlas, 0, &box, p + sizeof(r), r.width * 4, 0);
        }

        void onSection(ID3D11Device* device, const std::uint8_t* p, std::uint32_t bytes) {
            proto::RenSection hdr{};
            std::memcpy(&hdr, p, sizeof(hdr));
            const auto k = key(hdr.sx, hdr.sy, hdr.sz);
            if (auto it = sections.find(k); it != sections.end()) {
                rel(it->second.vb);
                sections.erase(it);
            }
            const std::uint64_t vbBytes = std::uint64_t(hdr.vertexCount) * sizeof(proto::RenVertex);
            if (hdr.vertexCount == 0 || bytes < sizeof(hdr) + vbBytes)
                return;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = UINT(vbBytes);
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA init{ p + sizeof(hdr), 0, 0 };
            Section s{ nullptr, hdr.vertexCount, hdr.sx, hdr.sy, hdr.sz };
            if (SUCCEEDED(device->CreateBuffer(&bd, &init, &s.vb)))
                sections.emplace(k, s);
        }

        // dynTris, dynCracks, dynLines into one dynamic vertex buffer, in that order.
        bool uploadDynamic(ID3D11Device* device, ID3D11DeviceContext* context) {
            const UINT bytes = UINT((dynTris.size() + dynCracks.size() + dynLines.size()) * sizeof(proto::RenVertex));
            if (!bytes)
                return false;
            if (bytes > dynCapacity) {
                rel(dynVb);
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = std::max<UINT>(bytes * 2, 64 * 1024);
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (FAILED(device->CreateBuffer(&bd, nullptr, &dynVb))) {
                    dynCapacity = 0;
                    return false;
                }
                dynCapacity = bd.ByteWidth;
            }
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(context->Map(dynVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                return false;
            auto* dst = static_cast<std::uint8_t*>(m.pData);
            for (const auto* v : { &dynTris, &dynCracks, &dynLines }) {
                std::memcpy(dst, v->data(), v->size() * sizeof(proto::RenVertex));
                dst += v->size() * sizeof(proto::RenVertex);
            }
            context->Unmap(dynVb, 0);
            return true;
        }

        bool setOrigin(ID3D11DeviceContext* context, double mx, double my, double mz) {
            const auto origin = Coords::toHalo(mx, my, mz);
            const SectionCB sc{ { origin.x, origin.y, origin.z, 0.0f } };
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(context->Map(sectionCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                return false;
            std::memcpy(m.pData, &sc, sizeof(sc));
            context->Unmap(sectionCb, 0);
            return true;
        }

        void onTexture(ID3D11Device* device, const std::uint8_t* p, std::uint32_t bytes) {
            proto::RenTexture hdr{};
            std::memcpy(&hdr, p, sizeof(hdr));
            if (!hdr.id || bytes < sizeof(hdr) + std::uint64_t(hdr.width) * hdr.height * 4)
                return;
            auto& t = entityTextures[hdr.id];
            rel(t.srv);
            rel(t.tex);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = hdr.width;
            td.Height = hdr.height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA init{ p + sizeof(hdr), hdr.width * 4, 0 };
            if (SUCCEEDED(device->CreateTexture2D(&td, &init, &t.tex)))
                device->CreateShaderResourceView(t.tex, nullptr, &t.srv);
        }

        // [origin (3 doubles), scene only] batchCount, vertexCount, RenBatch[], RenVertex[].
        void onMesh(Mesh& m, const std::uint8_t* p, std::uint32_t bytes, bool withOrigin) {
            std::size_t at = 0;
            if (withOrigin) {
                if (bytes < 24)
                    return;
                std::memcpy(m.origin, p, 24);
                at = 24;
            }
            std::uint32_t counts[2]{};
            if (bytes < at + 8)
                return;
            std::memcpy(counts, p + at, 8);
            at += 8;
            const std::uint64_t need = at + std::uint64_t(counts[0]) * sizeof(proto::RenBatch) + std::uint64_t(counts[1]) * sizeof(proto::RenVertex);
            if (bytes < need)
                return;
            m.batches.resize(counts[0]);
            std::memcpy(m.batches.data(), p + at, counts[0] * sizeof(proto::RenBatch));
            at += counts[0] * sizeof(proto::RenBatch);
            m.verts.resize(counts[1]);
            std::memcpy(m.verts.data(), p + at, counts[1] * sizeof(proto::RenVertex));
            // Our pixel shader picks the pass per vertex: mark translucent batches' vertices.
            for (const auto& b : m.batches)
                if (b.flags & 1)
                    for (std::uint32_t i = b.first; i < b.first + b.count && i < counts[1]; ++i)
                        m.verts[i].flags |= 2;
            m.dirty = true;
        }

        bool uploadMesh(ID3D11Device* device, ID3D11DeviceContext* context, Mesh& m) {
            if (m.verts.empty())
                return false;
            if (!m.dirty)
                return m.vb != nullptr;
            m.dirty = false;
            const UINT bytes = UINT(m.verts.size() * sizeof(proto::RenVertex));
            if (bytes > m.capacity) {
                rel(m.vb);
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = std::max<UINT>(bytes * 2, 64 * 1024);
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                m.capacity = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &m.vb)) ? bd.ByteWidth : 0;
            }
            D3D11_MAPPED_SUBRESOURCE map{};
            if (!m.vb || FAILED(context->Map(m.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
                return false;
            std::memcpy(map.pData, m.verts.data(), bytes);
            context->Unmap(m.vb, 0);
            return true;
        }

        void drawMesh(ID3D11DeviceContext* context, const Mesh& m) {
            const UINT stride = sizeof(proto::RenVertex), offset = 0;
            context->IASetVertexBuffers(0, 1, &m.vb, &stride, &offset);
            for (const auto& b : m.batches) {
                ID3D11ShaderResourceView* srv = atlasSrv;
                if (b.texture) {
                    const auto it = entityTextures.find(b.texture);
                    if (it == entityTextures.end() || !it->second.srv)
                        continue;
                    srv = it->second.srv;
                }
                context->PSSetShaderResources(0, 1, &srv);
                context->Draw(b.count, b.first);
            }
            context->PSSetShaderResources(0, 1, &atlasSrv);
        }

        // The dug boxes around the player, rebuilt when digging changes or the player has moved on.
        void buildHoles(const double anchor[3]) {
            const double o[3] = { std::floor(anchor[0]), std::floor(anchor[1]), std::floor(anchor[2]) };
            if (!holesDirty && std::abs(o[0] - holeOrigin[0]) < 8 && std::abs(o[1] - holeOrigin[1]) < 8 && std::abs(o[2] - holeOrigin[2]) < 8)
                return;
            holesDirty = false;
            std::copy(o, o + 3, holeOrigin);
            holeVerts.clear();
            if (!Dig::any())
                return;
            const float lo[3] = { float(o[0] - kHoleReach), float(o[1] - kHoleReachY), float(o[2] - kHoleReach) };
            const float hi[3] = { float(o[0] + kHoleReach), float(o[1] + kHoleReachY), float(o[2] + kHoleReach) };
            std::vector<skycraft::Clip::Cube> cubes;
            Dig::collect(lo, hi, cubes);
            for (auto& c : cubes)  // relative, so the boxes' floats stay exact far from the origin
                c = { c[0] - int(o[0]), c[1] - int(o[1]), c[2] - int(o[2]) };
            static constexpr int kQuads[6][4] = { { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 5, 7, 6 } };
            constexpr float kGrow = 0.01f;  // drawn a little bigger than the test in the shader
            for (const auto& b : skycraft::Clip::Merge(cubes)) {
                HoleVertex corner[8];
                for (int i = 0; i < 8; ++i)
                    for (int k = 0; k < 3; ++k) {
                        corner[i].pos[k] = ((i >> k) & 1) ? b.hi[k] + kGrow : b.lo[k] - kGrow;
                        corner[i].lo[k] = b.lo[k];
                        corner[i].hi[k] = b.hi[k];
                    }
                for (const auto& q : kQuads)
                    for (int i : { q[0], q[1], q[2], q[0], q[2], q[3] })
                        holeVerts.push_back(corner[i]);
            }
        }

        // Halo's depth copied where the hole shader can read it, and the boxes uploaded. Before
        // Halo's depth is bound as our target.
        bool prepareHoles(ID3D11Device* device, ID3D11DeviceContext* context, const double anchor[3]) {
            buildHoles(anchor);
            if (holeVerts.empty())
                return false;
            D3D11_TEXTURE2D_DESC d{};
            haloDepth->GetDesc(&d);
            if (d.SampleDesc.Count != 1)
                return false;  // ponytail: no holes with MSAA (Halo CE in MCC doesn't use it)
            D3D11_TEXTURE2D_DESC have{};
            if (depthCopy)
                depthCopy->GetDesc(&have);
            if (!depthCopy || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
                rel(depthCopySrv);
                rel(depthCopy);
                d.Usage = D3D11_USAGE_DEFAULT;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                d.CPUAccessFlags = 0;
                d.MiscFlags = 0;
                D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D.MipLevels = 1;
                if (FAILED(device->CreateTexture2D(&d, nullptr, &depthCopy)) || FAILED(device->CreateShaderResourceView(depthCopy, &sv, &depthCopySrv))) {
                    log("dug holes: no copy of Halo's depth");
                    rel(depthCopy);
                    return false;
                }
            }
            const UINT bytes = UINT(holeVerts.size() * sizeof(HoleVertex));
            if (bytes > holeCapacity) {
                rel(holeVb);
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = std::max<UINT>(bytes * 2, 64 * 1024);
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                holeCapacity = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &holeVb)) ? bd.ByteWidth : 0;
            }
            D3D11_MAPPED_SUBRESOURCE m{};
            if (!holeVb || FAILED(context->Map(holeVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                return false;
            std::memcpy(m.pData, holeVerts.data(), bytes);
            context->Unmap(holeVb, 0);
            context->OMSetRenderTargets(0, nullptr, nullptr);  // Halo's depth may still be bound
            context->CopyResource(depthCopy, haloDepth);
            return true;
        }

        void setPass(ID3D11DeviceContext* context, FrameCB& f, float pass) {
            f.misc[2] = pass;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(context->Map(frameCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                std::memcpy(m.pData, &f, sizeof(f));
                context->Unmap(frameCb, 0);
            }
        }
    }

    void drain(ID3D11Device* device, ID3D11DeviceContext* context) {
        if (!init(device))
            return;
        Link::drainRender([&](std::uint32_t type, const std::uint8_t* p, std::uint32_t bytes) {
            switch (type) {
            case proto::kRenAtlas: onAtlas(device, p, bytes); break;
            case proto::kRenAtlasRegion: onAtlasRegion(context, p, bytes); break;
            case proto::kRenSection: onSection(device, p, bytes); break;
            case proto::kRenClearAll:
                dropSections();
                Dig::clear();
                holesDirty = true;
                break;
            case proto::kRenDug:
                Dig::onDug(p, bytes);
                holesDirty = true;
                break;
            case proto::kRenTexture: onTexture(device, p, bytes); break;
            case proto::kRenAvatar: onMesh(avatar, p, bytes, false); break;
            case proto::kRenScene: onMesh(scene, p, bytes, true); break;
            default: break;  // ponytail: block lights, NPC solids, ragdoll: not used yet
            }
        }, kDrainBytesPerFrame);
    }

    void onSceneRendered() {
        if (!vs)
            return;
        ID3D11Device* device = nullptr;
        frameCb->GetDevice(&device);
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);

        // Halo's view-projection, exactly as it drew this frame's level: GPU-to-GPU, no stall.
        ID3D11Buffer* haloCb = nullptr;
        context->VSGetConstantBuffers(0, 1, &haloCb);
        const bool copied = haloCb != nullptr;
        if (haloCb) {
            const D3D11_BOX box{ 0, 0, 0, kViewProjBytes, 1, 1 };
            context->CopySubresourceRegion(viewProjCb, 0, 0, 0, 0, haloCb, 0, &box);
            rel(haloCb);
        }

        // And its depth buffer (kept across frames until Halo swaps it for another).
        ID3D11DepthStencilView* sceneDsv = nullptr;
        context->OMGetRenderTargets(0, nullptr, &sceneDsv);
        if (sceneDsv) {
            ID3D11Resource* res = nullptr;
            sceneDsv->GetResource(&res);
            ID3D11Texture2D* tex = nullptr;
            if (res && SUCCEEDED(res->QueryInterface(&tex)) && tex != haloDepth) {
                rel(haloDsv);
                rel(haloDepth);
                haloDepth = tex;
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
                dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;  // Halo's is R32G8X24_TYPELESS
                dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                const HRESULT hr = device->CreateDepthStencilView(tex, &dv, &haloDsv);
                haloDepthW = d.Width;
                haloDepthH = d.Height;
                log(std::format("Halo depth {}x{} format {}: {}", d.Width, d.Height, int(d.Format), SUCCEEDED(hr) ? "blocks hide behind Halo" : "no depth view"));
            } else if (tex) {
                tex->Release();
            }
            rel(res);
            sceneDsv->Release();
        }
        sceneSeen = copied;
        context->Release();
        device->Release();
    }

    void draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, unsigned width, unsigned height, const double anchor[3]) {
        const bool sceneDrawn = std::exchange(sceneSeen, false);
        if (!vs || !atlasSrv || !sceneDrawn || !haloDsv || haloDepthW != width || haloDepthH != height)
            return;  // ponytail: no fallback when Halo renders at another resolution than the window

        // Entities relative to a whole block near the player, so floats stay exact.
        const double origin[3] = { std::floor(anchor[0]), std::floor(anchor[1]), std::floor(anchor[2]) };
        dynTris.clear();
        dynCracks.clear();
        dynLines.clear();
        if (Link::readWorldEntities(entities))
            WorldEntities::build(entities, origin, dynTris, dynCracks, dynLines);
        const bool haveDynamic = uploadDynamic(device, context);
        const bool haveAvatar = uploadMesh(device, context, avatar);
        const bool haveScene = uploadMesh(device, context, scene);
        if (sections.empty() && !haveDynamic && !haveAvatar && !haveScene)
            return;

        FrameCB fc{};
        fc.misc[0] = Coords::kBlocksPerUnit;
        fc.misc[1] = 1.0f;  // ponytail: always day; Halo's lightmaps could drive this later

        const bool holes = prepareHoles(device, context, anchor);
        context->OMSetRenderTargets(1, &rtv, haloDsv);
        const D3D11_VIEWPORT vp{ 0, 0, float(width), float(height), 0, 1 };
        context->RSSetViewports(1, &vp);
        context->RSSetState(raster);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* cbs[3] = { frameCb, sectionCb, viewProjCb };
        context->VSSetConstantBuffers(0, 3, cbs);
        context->PSSetConstantBuffers(0, 3, cbs);
        context->PSSetSamplers(0, 1, &sampler);
        const float factor[4]{};

        // Dug holes first: Halo's ground in them is cleared, so the blocks below draw in them.
        if (holes && setOrigin(context, holeOrigin[0], holeOrigin[1], holeOrigin[2])) {
            setPass(context, fc, 0.0f);
            context->IASetInputLayout(holeLayout);
            context->VSSetShader(holeVs, nullptr, 0);
            context->PSSetShader(holePs, nullptr, 0);
            context->PSSetShaderResources(1, 1, &depthCopySrv);
            context->OMSetBlendState(opaque, factor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(depthOverwrite, 0);
            const UINT stride = sizeof(HoleVertex), offset = 0;
            context->IASetVertexBuffers(0, 1, &holeVb, &stride, &offset);
            context->Draw(UINT(holeVerts.size()), 0);
            ID3D11ShaderResourceView* none = nullptr;
            context->PSSetShaderResources(1, 1, &none);
        }

        context->IASetInputLayout(layout);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->PSSetShaderResources(0, 1, &atlasSrv);
        for (int pass = 0; pass < 2; ++pass) {
            setPass(context, fc, float(pass));
            context->OMSetBlendState(pass ? alphaBlend : opaque, factor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(pass ? depthTest : depthWrite, 0);
            const UINT stride = sizeof(proto::RenVertex), offset = 0;
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (const auto& [k, s] : sections) {
                if (!setOrigin(context, s.sx * 16.0, s.sy * 16.0, s.sz * 16.0))
                    continue;
                context->IASetVertexBuffers(0, 1, &s.vb, &stride, &offset);
                context->Draw(s.count, 0);
            }
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (haveAvatar && setOrigin(context, anchor[0], anchor[1], anchor[2]))  // Steve's feet
                drawMesh(context, avatar);
            if (haveScene && setOrigin(context, scene.origin[0], scene.origin[1], scene.origin[2]))
                drawMesh(context, scene);
            if (haveDynamic && setOrigin(context, origin[0], origin[1], origin[2])) {
                context->IASetVertexBuffers(0, 1, &dynVb, &stride, &offset);
                if (!dynTris.empty())
                    context->Draw(UINT(dynTris.size()), 0);
                if (!dynLines.empty()) {
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                    context->Draw(UINT(dynLines.size()), UINT(dynTris.size() + dynCracks.size()));
                }
            }
        }
        // Cracks last, over everything they sit on (translucent pass constants still bound).
        if (haveDynamic && !dynCracks.empty()) {
            context->OMSetBlendState(crumble, factor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(depthTest, 0);
            context->RSSetState(biased);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->Draw(UINT(dynCracks.size()), UINT(dynTris.size()));
        }
    }

    void release() {
        dropSections();
        rel(haloDsv), rel(haloDepth), rel(dynVb);
        dynCapacity = 0;
        for (auto* m : { &avatar, &scene }) {
            rel(m->vb);
            *m = Mesh{};
        }
        for (auto& [id, t] : entityTextures) {
            rel(t.srv);
            rel(t.tex);
        }
        entityTextures.clear();
        rel(atlasSrv), rel(atlas), rel(vs), rel(ps), rel(layout), rel(frameCb), rel(sectionCb), rel(viewProjCb);
        rel(sampler), rel(raster), rel(depthWrite), rel(depthTest), rel(opaque), rel(alphaBlend), rel(crumble), rel(biased);
        rel(holeVs), rel(holePs), rel(holeLayout), rel(depthOverwrite), rel(depthCopySrv), rel(depthCopy), rel(holeVb);
        holeCapacity = 0;
        holeVerts.clear();
        holesDirty = true;
        atlasW = atlasH = haloDepthW = haloDepthH = 0;
        sceneSeen = false;
        initFailed = false;
    }
}
