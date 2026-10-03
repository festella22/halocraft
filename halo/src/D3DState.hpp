#pragma once
#include <d3d11.h>

// Everything our Present-time draws touch, saved before and restored after, so Halo's (and Spark's)
// renderer never notices us.
struct D3DState {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4]{};
    UINT sampleMask = 0;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    UINT vbStride = 0, vbOffset = 0;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* vsCb[3]{};
    ID3D11Buffer* psCb[3]{};
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* sampler = nullptr;

    void save(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->RSGetState(&raster);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->RSGetViewports(&viewportCount, viewports);
        c->IAGetPrimitiveTopology(&topology);
        c->IAGetInputLayout(&layout);
        c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->VSGetConstantBuffers(0, 3, vsCb);
        c->PSGetConstantBuffers(0, 3, psCb);
        c->PSGetShaderResources(0, 1, &srv);
        c->PSGetSamplers(0, 1, &sampler);
    }

    void restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->RSSetState(raster);
        c->OMSetDepthStencilState(depth, stencilRef);
        c->RSSetViewports(viewportCount, viewports);
        c->IASetPrimitiveTopology(topology);
        c->IASetInputLayout(layout);
        c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->VSSetConstantBuffers(0, 3, vsCb);
        c->PSSetConstantBuffers(0, 3, psCb);
        c->PSSetShaderResources(0, 1, &srv);
        c->PSSetSamplers(0, 1, &sampler);
        auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
        for (auto*& r : rtv)
            rel(r);
        rel(dsv), rel(blend), rel(raster), rel(depth), rel(layout), rel(vb), rel(vs), rel(ps), rel(srv), rel(sampler);
        for (auto*& b : vsCb)
            rel(b);
        for (auto*& b : psCb)
            rel(b);
    }
};
