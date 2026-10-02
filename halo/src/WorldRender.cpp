#include "WorldRender.hpp"
#define NOMINMAX
#include <Windows.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include "Coords.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "engine/player.hpp"

namespace WorldRender {
    namespace {
        namespace proto = skycraft::proto;

        constexpr float kNear = 0.01f, kFar = 2000.0f;  // Halo units
        constexpr std::uint64_t kDrainBytesPerFrame = 48ull << 20;  // the 21 MB atlas must fit in one go

        struct Section {
            ID3D11Buffer* vb = nullptr;
            UINT count = 0;
            int sx, sy, sz;
        };

        struct alignas(16) FrameCB {
            float right[4], up[4], fwd[4];
            float proj[4];  // 1/tan(hfov/2), 1/tan(vfov/2), depth scale, depth bias
            float misc[4];  // blocks per unit, daylight, pass (0 solid, 1 translucent), -
        };
        struct alignas(16) SectionCB {
            float offset[4];  // section origin relative to the camera, Halo units
        };

        std::unordered_map<std::uint64_t, Section> sections;
        ID3D11Texture2D* atlas = nullptr;
        ID3D11ShaderResourceView* atlasSrv = nullptr;
        UINT atlasW = 0, atlasH = 0;
        ID3D11Texture2D* depthTex = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        UINT depthW = 0, depthH = 0;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11InputLayout* layout = nullptr;
        ID3D11Buffer* frameCb = nullptr;
        ID3D11Buffer* sectionCb = nullptr;
        ID3D11SamplerState* sampler = nullptr;
        ID3D11RasterizerState* raster = nullptr;
        ID3D11DepthStencilState* depthWrite = nullptr;
        ID3D11DepthStencilState* depthTest = nullptr;
        ID3D11BlendState* opaque = nullptr;
        ID3D11BlendState* alphaBlend = nullptr;
        bool initFailed = false;

        constexpr char kShader[] = R"(
cbuffer Frame : register(b0) { float4 camRight; float4 camUp; float4 camFwd; float4 proj; float4 misc; };
cbuffer Section : register(b1) { float4 offset; };
Texture2D atlas : register(t0);
SamplerState samp : register(s0);
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; uint light : TEXCOORD1; uint flags : TEXCOORD2; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; float2 light : TEXCOORD1; nointerpolation uint flags : TEXCOORD2; };
VSOut VSMain(VSIn i) {
	VSOut o;
	float3 rel = offset.xyz + float3(i.pos.x, -i.pos.z, i.pos.y) / misc.x;  // Minecraft -> Halo axes
	float w = dot(rel, camFwd.xyz);
	o.pos = float4(dot(rel, camRight.xyz) * proj.x, dot(rel, camUp.xyz) * proj.y, w * proj.z + proj.w, w);
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
	float4 tex = atlas.Sample(samp, i.uv);
	bool translucent = (i.flags & 2) != 0;
	if (translucent != (misc.z > 0.5)) discard;
	if ((i.flags & 1) != 0 && tex.a < 0.1) discard;  // cutout: leaves, glass panes, flowers
	float light = max(Curve(i.light.x), Curve(i.light.y) * misc.y);
	float3 rgb = tex.rgb * i.color.rgb * kShade[(i.flags >> 4) & 7] * lerp(0.08, 1.0, light);
	return float4(rgb, translucent ? tex.a * i.color.a : 1.0);
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
            auto compile = [&](const char* entry, const char* target, ID3DBlob** out) {
                const auto hr = D3DCompile(kShader, sizeof(kShader) - 1, "halocraft_blocks", nullptr, nullptr, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
                if (FAILED(hr))
                    log(std::string("block shader ") + entry + " failed: " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?"));
                rel(errors);
                return SUCCEEDED(hr);
            };
            if (!compile("VSMain", "vs_5_0", &vsBlob) || !compile("PSMain", "ps_5_0", &psBlob)) {
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

            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            device->CreateSamplerState(&sd, &sampler);

            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;  // ponytail: no back-face culling; winding isn't pinned down yet
            rd.DepthClipEnable = TRUE;
            device->CreateRasterizerState(&rd, &raster);

            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = TRUE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            device->CreateDepthStencilState(&dd, &depthWrite);
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            device->CreateDepthStencilState(&dd, &depthTest);

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

            const bool ok = vs && ps && layout && frameCb && sectionCb && sampler && raster && depthWrite && depthTest && opaque && alphaBlend;
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

        bool ensureDepth(ID3D11Device* device, UINT w, UINT h) {
            if (dsv && depthW == w && depthH == h)
                return true;
            rel(dsv);
            rel(depthTex);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_D32_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &depthTex)) || FAILED(device->CreateDepthStencilView(depthTex, nullptr, &dsv)))
                return false;
            depthW = w;
            depthH = h;
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
            case proto::kRenClearAll: dropSections(); break;
            default: break;  // ponytail: avatar, entities, lights, solids, dug: not drawn yet
            }
        }, kDrainBytesPerFrame);
    }

    void draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, unsigned width, unsigned height) {
        if (!vs || !atlasSrv || sections.empty() || !ensureDepth(device, width, height))
            return;
        const auto* cam = Engine::getPlayerCameraPointer();
        if (!cam || !std::isfinite(cam->fov) || cam->fov <= 0.0f)
            return;

        // Halo's camera basis (right-handed, Z up): right = forward x up.
        const float f[3] = { cam->fwd.x, cam->fwd.y, cam->fwd.z };
        float u[3] = { cam->up.x, cam->up.y, cam->up.z };
        float r[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };
        const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (rl < 1e-6f)
            return;
        for (float& c : r)
            c /= rl;
        u[0] = r[1] * f[2] - r[2] * f[1], u[1] = r[2] * f[0] - r[0] * f[2], u[2] = r[0] * f[1] - r[1] * f[0];

        FrameCB fc{};
        std::memcpy(fc.right, r, sizeof(r));
        std::memcpy(fc.up, u, sizeof(u));
        std::memcpy(fc.fwd, f, sizeof(f));
        const float tanH = std::tan(cam->fov * 0.5f);  // Halo's FOV is horizontal
        fc.proj[0] = 1.0f / tanH;
        fc.proj[1] = float(width) / (float(height) * tanH);
        fc.proj[2] = kFar / (kFar - kNear);
        fc.proj[3] = -kNear * kFar / (kFar - kNear);
        fc.misc[0] = Coords::kBlocksPerUnit;
        fc.misc[1] = 1.0f;  // ponytail: always day; Halo's lightmaps could drive this later

        context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        // ponytail: our own depth buffer only, so Halo's walls don't hide blocks behind them yet.
        context->OMSetRenderTargets(1, &rtv, dsv);
        const D3D11_VIEWPORT vp{ 0, 0, float(width), float(height), 0, 1 };
        context->RSSetViewports(1, &vp);
        context->RSSetState(raster);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(layout);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        ID3D11Buffer* cbs[2] = { frameCb, sectionCb };
        context->VSSetConstantBuffers(0, 2, cbs);
        context->PSSetConstantBuffers(0, 2, cbs);
        context->PSSetShaderResources(0, 1, &atlasSrv);
        context->PSSetSamplers(0, 1, &sampler);

        const float factor[4]{};
        for (int pass = 0; pass < 2; ++pass) {
            setPass(context, fc, float(pass));
            context->OMSetBlendState(pass ? alphaBlend : opaque, factor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(pass ? depthTest : depthWrite, 0);
            for (const auto& [k, s] : sections) {
                // Section origin relative to the camera, in Halo units.
                const auto origin = Coords::toHalo(s.sx * 16.0, s.sy * 16.0, s.sz * 16.0);
                SectionCB sc{};
                sc.offset[0] = origin.x - cam->pos.x;
                sc.offset[1] = origin.y - cam->pos.y;
                sc.offset[2] = origin.z - cam->pos.z;
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(context->Map(sectionCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                    continue;
                std::memcpy(m.pData, &sc, sizeof(sc));
                context->Unmap(sectionCb, 0);
                const UINT stride = sizeof(proto::RenVertex), offset = 0;
                context->IASetVertexBuffers(0, 1, &s.vb, &stride, &offset);
                context->Draw(s.count, 0);
            }
        }
    }

    void release() {
        dropSections();
        rel(atlasSrv), rel(atlas), rel(dsv), rel(depthTex), rel(vs), rel(ps), rel(layout), rel(frameCb), rel(sectionCb);
        rel(sampler), rel(raster), rel(depthWrite), rel(depthTest), rel(opaque), rel(alphaBlend);
        atlasW = atlasH = depthW = depthH = 0;
        initFailed = false;
    }
}
