#pragma once
#include <d3d11.h>

// Minecraft's blocks drawn in Halo's world. Minecraft meshes its own chunk sections (models, tint,
// ambient occlusion, light levels) and ships them with its block atlas through the render ring;
// this keeps them on the GPU and draws them with Halo's camera.
namespace WorldRender {
    // Present, every frame: take Minecraft's updates (it stalls if the ring fills up).
    void drain(ID3D11Device* device, ID3D11DeviceContext* context);
    // Present, before the hand/HUD layer. Pipeline state is saved/restored by the caller.
    // anchor: Minecraft's player (Minecraft coordinates), to keep per-frame geometry precise.
    void draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, unsigned width, unsigned height,
        const double anchor[3]);
    void release();

    // Halo's render thread, right after it draws the level: look at the scene's depth buffer.
    void onSceneRendered();
}
