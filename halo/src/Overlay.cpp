// Draws Minecraft's GUI layer (hand, hotbar, hearts, open screens) into Halo's own frame, and keeps
// the link alive. Ported from SkyCraft's skse/src/Overlay.cpp (MIT, chasmlol): same shader, blend
// and triple-buffer upload. Differences: Present is found through a throwaway swap chain (Spark
// owns the game's), and Halo's state is still a stub until Phase 2.
#include "Overlay.hpp"
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <string>
#include "D3DState.hpp"
#include "Input.hpp"
#include "Link.hpp"
#include "WorldRender.hpp"
#include "Log.hpp"
#include "Player.hpp"
#include "engine/halo1.hpp"
#include "spark/mod/ImGuiBridge.hpp"

namespace Overlay {
    namespace {
        namespace proto = skycraft::proto;

        using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
        using Present1Fn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, const void*);
        constexpr int kPresent = 8, kPresent1 = 22;  // IDXGISwapChain / IDXGISwapChain1 vtable slots

        void** vtable = nullptr;
        PresentFn originalPresent = nullptr;
        Present1Fn originalPresent1 = nullptr;

        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        UINT texW = 0, texH = 0;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11PixelShader* psInvert = nullptr;
        ID3D11BlendState* blend = nullptr;
        ID3D11BlendState* invertBlend = nullptr;
        ID3D11SamplerState* sampler = nullptr;
        ID3D11RasterizerState* raster = nullptr;
        ID3D11DepthStencilState* depth = nullptr;
        ID3D11Buffer* params = nullptr;
        bool haveFrame = false, flipY = true, initFailed = false;

        struct alignas(16) Params {
            float cursor[2];
            float viewport[2];
            float cursorOn;
            float flipY;
            float scope;  // bow zoom 0..1: darken outside a sniper-scope circle
            float pad;
            float invertRect[4];  // back buffer pixels: x0, y0, x1, y1 (empty: none)
        };

        constexpr char kShader[] = R"(
cbuffer Params : register(b0) { float2 cursor; float2 viewport; float cursorOn; float flipY; float scope; float pad; float4 invertRect; };
Texture2D overlay : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
bool InInvertRect(float2 p) { return all(p >= invertRect.xy) && all(p < invertRect.zw); }
float4 Overlay(float2 uv) {
	if (flipY > 0.5) uv.y = 1 - uv.y;
	return overlay.Sample(samp, uv);   // premultiplied alpha straight from Minecraft
}
// Minecraft's crosshair and attack indicator use its invert blend against the game's picture.
float4 PSInvert(VSOut i) : SV_Target {
	if (!InInvertRect(i.pos.xy)) discard;
	return float4(Overlay(i.uv).rgb, 0);
}
float4 PSMain(VSOut i) : SV_Target {
	if (InInvertRect(i.pos.xy)) return 0;
	float4 c = Overlay(i.uv);
	// The bow's scope: black outside a circle, under Minecraft's HUD (premultiplied: add coverage).
	if (scope > 0) {
		float d = length(i.pos.xy - viewport * 0.5) / (viewport.y * 0.47);
		c.a += (1 - c.a) * scope * smoothstep(0.97, 1.0, d);
	}
	if (cursorOn > 0.5) {
		float2 p = i.pos.xy - cursor;
		if (p.x >= 0 && p.y >= 0 && p.y < 18 && p.x <= p.y * 0.6) {
			bool edge = p.x < 1.5 || p.x > p.y * 0.6 - 1.5 || p.y > 16.5;
			c = float4(edge ? float3(0, 0, 0) : float3(1, 1, 1), 1);
		}
	}
	return c;
}
)";

        template <class T> void release(T*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        }

        bool compile(const char* entry, const char* target, ID3DBlob** out) {
            ID3DBlob* errors = nullptr;
            const auto hr = D3DCompile(kShader, sizeof(kShader) - 1, "halocraft_overlay", nullptr, nullptr, entry, target,
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
            if (FAILED(hr))
                log(std::string("overlay shader ") + entry + " failed: " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?"));
            release(errors);
            return SUCCEEDED(hr);
        }

        bool initResources(IDXGISwapChain* swapChain) {
            if (device)
                return true;
            if (initFailed || FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
                initFailed = true;
                return false;
            }
            device->GetImmediateContext(&context);

            ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *psInvertBlob = nullptr;
            if (!compile("VSMain", "vs_5_0", &vsBlob) || !compile("PSMain", "ps_5_0", &psBlob) || !compile("PSInvert", "ps_5_0", &psInvertBlob)) {
                initFailed = true;
                return false;
            }
            device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
            device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
            device->CreatePixelShader(psInvertBlob->GetBufferPointer(), psInvertBlob->GetBufferSize(), nullptr, &psInvert);
            release(vsBlob);
            release(psBlob);
            release(psInvertBlob);

            D3D11_BLEND_DESC bd{};
            auto& rt = bd.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_ONE;
            rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            device->CreateBlendState(&bd, &blend);
            // Minecraft's BlendFunction.INVERT; the back buffer's alpha is left alone.
            rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
            rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
            rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
            rt.DestBlendAlpha = D3D11_BLEND_ONE;
            device->CreateBlendState(&bd, &invertBlend);

            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            device->CreateSamplerState(&sd, &sampler);

            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            device->CreateRasterizerState(&rd, &raster);

            D3D11_DEPTH_STENCIL_DESC dd{};
            device->CreateDepthStencilState(&dd, &depth);

            D3D11_BUFFER_DESC cbd{};
            cbd.ByteWidth = sizeof(Params);
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&cbd, nullptr, &params);

            const bool ok = vs && ps && psInvert && blend && invertBlend && sampler && raster && depth && params;
            log(ok ? "overlay renderer ready" : "overlay renderer failed to initialize");
            initFailed = !ok;
            return ok;
        }

        bool ensureTexture(UINT w, UINT h) {
            if (texture && texW == w && texH == h)
                return true;
            release(srv);
            release(texture);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &texture)) || FAILED(device->CreateShaderResourceView(texture, nullptr, &srv))) {
                log("overlay texture creation failed");
                return false;
            }
            texW = w;
            texH = h;
            log("overlay texture " + std::to_string(w) + "x" + std::to_string(h));
            return true;
        }

        void uploadLatestFrame() {
            if (!Link::acquireOverlayFrame())
                return;
            const auto* hdr = Link::frontHeader();
            if (hdr->width == 0 || hdr->height == 0 || hdr->width > proto::kMaxOverlayW || hdr->height > proto::kMaxOverlayH)
                return;
            if (!ensureTexture(hdr->width, hdr->height))
                return;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return;
            const auto* src = Link::frontPixels();
            const UINT rowBytes = hdr->width * 4;
            auto* dst = static_cast<std::uint8_t*>(mapped.pData);
            if (mapped.RowPitch == rowBytes) {
                std::memcpy(dst, src, std::size_t(rowBytes) * hdr->height);
            } else {
                for (UINT y = 0; y < hdr->height; ++y)
                    std::memcpy(dst + std::size_t(y) * mapped.RowPitch, src + std::size_t(y) * rowBytes, rowBytes);
            }
            context->Unmap(texture, 0);
            flipY = (hdr->flags & 1) != 0;
            haveFrame = true;
        }

        void drawOverlay(ID3D11RenderTargetView* rtv, const D3D11_TEXTURE2D_DESC& bb, const proto::McState& mc) {
            const bool screenOpen = (mc.flags & proto::kMcScreenOpen) != 0;
            bool invert = false;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                auto* p = static_cast<Params*>(mapped.pData);
                // The cursor lives in overlay pixels; scale if the overlay and back buffer differ.
                const float sx = texW ? float(bb.Width) / float(texW) : 1.0f;
                const float sy = texH ? float(bb.Height) / float(texH) : 1.0f;
                p->cursor[0] = float(Input::cursorX.load()) * sx;
                p->cursor[1] = float(Input::cursorY.load()) * sy;
                p->viewport[0] = float(bb.Width);
                p->viewport[1] = float(bb.Height);
                p->cursorOn = screenOpen ? 1.0f : 0.0f;
                p->flipY = flipY ? 1.0f : 0.0f;
                p->scope = screenOpen ? 0.0f : std::clamp((Player::zoom() - 1.0f) / (Player::kMaxZoom - 1.0f) * 3.0f, 0.0f, 1.0f);
                // The crosshair (15 GUI px) and attack indicator below it, around the screen centre.
                const float g = float(mc.guiScale) * sx;
                const float cx = float(bb.Width) * 0.5f, cy = float(bb.Height) * 0.5f;
                invert = !screenOpen && mc.guiScale > 0;
                p->invertRect[0] = invert ? cx - 12.0f * g : 0.0f;
                p->invertRect[1] = invert ? cy - 12.0f * g : 0.0f;
                p->invertRect[2] = invert ? cx + 12.0f * g : 0.0f;
                p->invertRect[3] = invert ? cy + 28.0f * g : 0.0f;
                context->Unmap(params, 0);
            }

            D3D11_VIEWPORT vp{ 0, 0, float(bb.Width), float(bb.Height), 0, 1 };
            const float factor[4]{};
            context->OMSetRenderTargets(1, &rtv, nullptr);
            context->OMSetBlendState(blend, factor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(depth, 0);
            context->RSSetState(raster);
            context->RSSetViewports(1, &vp);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->IASetInputLayout(nullptr);
            context->VSSetShader(vs, nullptr, 0);
            context->PSSetShader(ps, nullptr, 0);
            context->PSSetShaderResources(0, 1, &srv);
            context->PSSetSamplers(0, 1, &sampler);
            context->PSSetConstantBuffers(0, 1, &params);
            context->Draw(3, 0);
            if (invert) {
                context->OMSetBlendState(invertBlend, factor, 0xFFFFFFFF);
                context->PSSetShader(psInvert, nullptr, 0);
                context->Draw(3, 0);
            }

        }

        // Every Halo frame: tell Minecraft we're alive and what to render at, then draw its frame.
        // Present runs even while paused, so this is where the heartbeat lives.
        void frame(IDXGISwapChain* swapChain) {
            Link::heartbeat();

            // Spark sets ImGui's WantCaptureMouse exactly while Halo's pause menu is open.
            Spark::Mod::syncImGuiContext();
            const bool paused = ImGui::GetIO().WantCaptureMouse;

            Input::haloPaused = paused;
            proto::McState mc{};
            const bool live = Link::mcAlive() && Link::readMcState(mc) && (mc.flags & proto::kMcInWorld);
            Input::mcScreenOpen = live && (mc.flags & proto::kMcScreenOpen);

            DXGI_SWAP_CHAIN_DESC desc{};
            swapChain->GetDesc(&desc);
            proto::SkyState sky{};
            sky.flags = Engine::isGameLoaded() ? proto::kSkyInGame : 0;
            if (paused)
                sky.flags |= proto::kSkyMenuOpen;
            sky.viewportW = desc.BufferDesc.Width;
            sky.viewportH = desc.BufferDesc.Height;
            // ponytail: Halo's player is read on the render thread; a torn read is one frame of jitter.
            Player::frame(sky, live ? &mc : nullptr);
            Link::writeSkyState(sky);

            if (!live || !initResources(swapChain))
                return;
            WorldRender::drain(device, context);  // always: Minecraft stalls when its ring fills up
            uploadLatestFrame();
            Input::overlayW = int(texW);
            Input::overlayH = int(texH);
            if (paused)
                return;

            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
                return;
            ID3D11RenderTargetView* rtv = nullptr;
            const auto hr = device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
            D3D11_TEXTURE2D_DESC bb{};
            backBuffer->GetDesc(&bb);
            release(backBuffer);
            if (FAILED(hr)) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    log("overlay: back buffer RTV failed (format " + std::to_string(bb.Format) + ")");
                }
                return;
            }
            D3DState saved;
            saved.save(context);
            const double anchor[3] = { mc.x, mc.y, mc.z };
            WorldRender::draw(device, context, rtv, bb.Width, bb.Height, anchor);  // blocks go under the hand and HUD
            if (haveFrame)
                drawOverlay(rtv, bb, mc);
            saved.restore(context);
            release(rtv);
        }

        HRESULT WINAPI presentHook(IDXGISwapChain* swapChain, UINT sync, UINT flags) {
            frame(swapChain);
            return originalPresent(swapChain, sync, flags);
        }

        HRESULT WINAPI present1Hook(IDXGISwapChain* swapChain, UINT sync, UINT flags, const void* presentParams) {
            frame(swapChain);
            return originalPresent1(swapChain, sync, flags, presentParams);
        }

        void patch(int slot, void* fn) {
            DWORD old = 0;
            VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
            vtable[slot] = fn;
            VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
        }
    }

    // Every DXGI swap chain shares one vtable, so a throwaway swap chain on a hidden window gets us
    // Halo's Present. Patching the vtable (not the code) leaves Spark's own hook on Present intact.
    bool install() {
        if (vtable)
            return true;
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"halocraft", 0, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        IDXGISwapChain* swapChain = nullptr;
        ID3D11Device* dummyDevice = nullptr;
        const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &sd, &swapChain, &dummyDevice, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            vtable = *reinterpret_cast<void***>(swapChain);
            originalPresent = reinterpret_cast<PresentFn>(vtable[kPresent]);
            originalPresent1 = reinterpret_cast<Present1Fn>(vtable[kPresent1]);
            patch(kPresent, reinterpret_cast<void*>(&presentHook));
            patch(kPresent1, reinterpret_cast<void*>(&present1Hook));
        }
        release(swapChain);
        release(dummyDevice);
        DestroyWindow(hwnd);
        log(vtable ? "Present hooked" : "Present hook failed (" + std::to_string(hr) + ")");
        return vtable != nullptr;
    }

    void uninstall() {
        if (!vtable)
            return;
        patch(kPresent, reinterpret_cast<void*>(originalPresent));
        patch(kPresent1, reinterpret_cast<void*>(originalPresent1));
        vtable = nullptr;
        // ponytail: a frame already inside frame() may still be running; wait it out instead of locking.
        Sleep(100);
        WorldRender::release();
        release(srv);
        release(texture);
        release(vs);
        release(ps);
        release(psInvert);
        release(blend);
        release(invertBlend);
        release(sampler);
        release(raster);
        release(depth);
        release(params);
        release(context);
        release(device);
        texW = texH = 0;
        haveFrame = false;
        initFailed = false;
    }
}
