// Text overlay drawn with D3D8, used by D3D8 games that never get a D3D9 device (dgVoodoo2)
// This mirrors d3d9_overlay.cpp - see the comment in overlay.h for why both exist

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include "common.h"
#include "overlay_font.h"
#include "overlay.h"
#include "d3d8_overlay.h"

D3D8Overlay* D3D8Overlay::Instance = nullptr;

struct CUSTOMVERTEX {
    float pos[3];
    D3DCOLOR col;
    float uv[2];
};
#define D3DFVF_CUSTOMVERTEX (D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1)

D3D8Overlay::D3D8Overlay(IDirect3DDevice8* device, int width, int height) {
    d3d8_device = device;
    window_width = width;
    window_height = height;
    memset(text_buffer, 0, sizeof(text_buffer));

    SetupRenderState();
    SetupResources();
    SetupAtlasUVTable();
}

D3D8Overlay::~D3D8Overlay() {
    if (d3d8_state_block != 0)
        d3d8_device->DeleteStateBlock(d3d8_state_block);
    if (d3d8_font_tex != nullptr)
        d3d8_font_tex->Release();
    if (d3d8_vertex_buf != nullptr)
        d3d8_vertex_buf->Release();
    if (d3d8_index_buf != nullptr)
        d3d8_index_buf->Release();
}

// Sets up the desired render state and caches it into a state block
void D3D8Overlay::SetupRenderState() {
    // The game might rely on default values during initialization, so we'll save the default state
    DWORD original_state = 0;
    d3d8_device->CreateStateBlock(D3DSBT_ALL, &original_state);
    d3d8_device->CaptureStateBlock(original_state);

    // Viewport and transforms. They are set again every time the overlay is drawn, since
    // state blocks don't carry the viewport around and the games change it (the pause screen
    // draws into a smaller area, which used to move the overlay along with it).
    SetupViewportAndTransforms();

    // Setup render state: fixed-pipeline, alpha-blending, no face culling, no depth testing,
    // fill mode, point sampling. D3D8 has no separate alpha blending and no sampler states,
    // the texture filter is part of the texture stage state here.
    d3d8_device->SetPixelShader(0);
    d3d8_device->SetVertexShader(D3DFVF_CUSTOMVERTEX);
    d3d8_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d3d8_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d3d8_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d3d8_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d3d8_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    d3d8_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d3d8_device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d3d8_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3d8_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    // (No scissor test: D3D8's header doesn't declare D3DRS_SCISSORTESTENABLE, and the
    // default scissor rect covers the whole render target anyway)
    d3d8_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d3d8_device->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    d3d8_device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    d3d8_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d3d8_device->SetRenderState(D3DRS_CLIPPING, TRUE);
    d3d8_device->SetRenderState(D3DRS_LIGHTING, FALSE);
    d3d8_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d3d8_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d3d8_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d3d8_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d3d8_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d3d8_device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d3d8_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3d8_device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d3d8_device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    d3d8_device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    d3d8_device->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_POINT);

    // Save the current state
    d3d8_state_block = 0;
    d3d8_device->CreateStateBlock(D3DSBT_ALL, &d3d8_state_block);
    d3d8_device->CaptureStateBlock(d3d8_state_block);

    // Restore the original state
    d3d8_device->ApplyStateBlock(original_state);
    d3d8_device->DeleteStateBlock(original_state);
}

// Sets up the viewport and the transforms the overlay geometry is drawn with
void D3D8Overlay::SetupViewportAndTransforms() {
    // Viewport
    D3DVIEWPORT8 viewport = {};
    viewport.Width = window_width;
    viewport.Height = window_height;
    viewport.X = 0;
    viewport.Y = 0;
    viewport.MinZ = 0.0;
    viewport.MaxZ = 0.0;
    d3d8_device->SetViewport(&viewport);

    // Orthographic projection matrix
    float L = 0.5f;
    float R = window_width + 0.5f;
    float T = 0.5f;
    float B = window_height + 0.5f;
    D3DMATRIX mat_identity = { { { 1.0f, 0.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f } } };
    D3DMATRIX mat_projection =
    { { {
        2.0f / (R - L),    0.0f,               0.0f,  0.0f,
        0.0f,              2.0f / (T - B),     0.0f,  0.0f,
        0.0f,              0.0f,               0.5f,  0.0f,
        (L + R) / (L - R), (T + B) / (B - T),  0.5f,  1.0f
    } } };
    d3d8_device->SetTransform(D3DTS_WORLD, &mat_identity);
    d3d8_device->SetTransform(D3DTS_VIEW, &mat_identity);
    d3d8_device->SetTransform(D3DTS_PROJECTION, &mat_projection);
}

// Decompresses and loads the font atlas to an A8R8G8B8 texture and sets up other resources
void D3D8Overlay::SetupResources() {
    // Create a temporary texture for uploading texture data from CPU
    IDirect3DTexture8* temp_tex;
    auto ret = d3d8_device->CreateTexture(FONT_ATLAS_WIDTH_P2, FONT_ATLAS_HEIGHT_P2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &temp_tex);
    if (FAILED(ret))
        panic_msgbox(L"Temporary font texture CreateTexture failed! Error: 0x%x", ret);

    // Map the texture into memory
    D3DLOCKED_RECT locked_rect;
    RECT target_rect = { 0, 0, FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT };
    ret = temp_tex->LockRect(0, &locked_rect, &target_rect, 0);
    if (FAILED(ret))
        panic_msgbox(L"Temporary font texture LockRect failed! Error: 0x%x", ret);

    // Decompress and load the font atlas
    size_t cur_pos = 0;
    char cur_byte = 0;
    unsigned bits_left = 0;
    for (int y = 0; y < FONT_ATLAS_HEIGHT; y++) {
        for (int x = 0; x < FONT_ATLAS_WIDTH; x++) {
            if (bits_left == 0) {
                cur_byte = compressed_font_atlas[cur_pos++];
                bits_left = 8;
            }
            ((uint32_t*)locked_rect.pBits)[y * (locked_rect.Pitch / 4) + x] = cur_byte & 0x80 ? 0xFFFFFFFF : 0x00000000;
            cur_byte <<= 1;
            bits_left -= 1;
        }
    }
    temp_tex->UnlockRect(0);

    // Create the final texture and copy the temporary texture to it
    ret = d3d8_device->CreateTexture(FONT_ATLAS_WIDTH_P2, FONT_ATLAS_HEIGHT_P2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &d3d8_font_tex);
    if (FAILED(ret))
        panic_msgbox(L"Font texture CreateTexture failed! Error: 0x%x", ret);
    d3d8_device->UpdateTexture(temp_tex, d3d8_font_tex);

    // Clean up
    temp_tex->Release();

    // Set up the vertex and index buffer
    ret = d3d8_device->CreateVertexBuffer((OVERLAY_MAX_CHARS + 1) * 4 * sizeof(CUSTOMVERTEX), D3DUSAGE_DYNAMIC,
        D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT, &d3d8_vertex_buf);
    if (FAILED(ret))
        panic_msgbox(L"CreateVertexBuffer failed! Error: 0x%x", ret);
    ret = d3d8_device->CreateIndexBuffer((OVERLAY_MAX_CHARS + 1) * 2 * 3 * sizeof(uint16_t), D3DUSAGE_DYNAMIC,
        D3DFMT_INDEX16, D3DPOOL_DEFAULT, &d3d8_index_buf);
    if (FAILED(ret))
        panic_msgbox(L"CreateIndexBuffer failed! Error: 0x%x", ret);
}

// Sets up the font atlas UV lookup table
void D3D8Overlay::SetupAtlasUVTable() {
    // Width of 8, height of 8, underhang of 4, and padding of 1
    for (int y = 0; y < 96 / 16; y++) {
        for (int x = 0; x < 16; x++) {
            atlas_uvs[y * 16 + x][0] = (x * 9.0f / (float)FONT_ATLAS_WIDTH) * (FONT_ATLAS_WIDTH / (float)FONT_ATLAS_WIDTH_P2);
            atlas_uvs[y * 16 + x][1] = (y * 13.0f / (float)FONT_ATLAS_HEIGHT) * (FONT_ATLAS_HEIGHT / (float)FONT_ATLAS_HEIGHT_P2);
        }
    }
    char_width = (8.0f / (float)FONT_ATLAS_WIDTH) * (FONT_ATLAS_WIDTH / (float)FONT_ATLAS_WIDTH_P2);
    char_height = (12.0f / (float)FONT_ATLAS_HEIGHT) * (FONT_ATLAS_HEIGHT / (float)FONT_ATLAS_HEIGHT_P2);
}

int D3D8Overlay::UpdateBuffers(char* text) {
    // Get the length of the string
    auto text_len = strlen(text);
    if (text_len > OVERLAY_MAX_CHARS)
        panic_msgbox(L"Tried to draw too many characters");

    // Lock the buffers for writing
    CUSTOMVERTEX* vertex_data;
    d3d8_vertex_buf->Lock(0, (text_len + 1) * 4 * sizeof(CUSTOMVERTEX), (BYTE**)&vertex_data, D3DLOCK_DISCARD);
    uint16_t* index_data;
    d3d8_index_buf->Lock(0, (text_len + 1) * 2 * 3 * sizeof(uint16_t), (BYTE**)&index_data, D3DLOCK_DISCARD);

    // Calculate the offsets
    float padding = 1.0f;
    float x_offset = 0;
    float y_offset = (float)window_height - 12.0f - padding * 2.0f;

    // Write the background geometry
    // HACK: The bottom right pixel of the atlas is set so I don't have to switch textures for this
    float epsilon = 0.0001f;
    float pixel_u = FONT_ATLAS_WIDTH / (float)FONT_ATLAS_WIDTH_P2 - epsilon;
    float pixel_v = FONT_ATLAS_HEIGHT / (float)FONT_ATLAS_HEIGHT_P2 - epsilon;
    auto bg_color = D3DCOLOR_COLORVALUE(0.0, 0.0, 0.0, 0.8);
    float bg_width = (float)text_len * 7.0f + padding * 2.0f;
    float bg_height = 12.0f + padding * 2.0f;
    vertex_data[0] = CUSTOMVERTEX{
        {x_offset + bg_width, y_offset + bg_height, 0.5},
        bg_color,
        {pixel_u + epsilon, pixel_v + epsilon}
    };
    vertex_data[1] = CUSTOMVERTEX{
        {x_offset + bg_width, y_offset, 0.5},
        bg_color,
        {pixel_u + epsilon, pixel_v}
    };
    vertex_data[2] = CUSTOMVERTEX{
        {x_offset, y_offset, 0.5},
        bg_color,
        {pixel_u, pixel_v}
    };
    vertex_data[3] = CUSTOMVERTEX{
        {x_offset, y_offset + bg_height, 0.5},
        bg_color,
        {pixel_u, pixel_v + epsilon}
    };
    index_data[0] = 0; index_data[1] = 1; index_data[2] = 3;
    index_data[3] = 1; index_data[4] = 2; index_data[5] = 3;

    // Write the text geometry
    size_t cur_vertex = 4;
    size_t cur_index = 6;
    float cur_xpos = 0.0;
    for (unsigned i = 0; i < text_len; i++) {
        // Ensure that this character is actually ASCII text
        // Incorrectly skipped but this is just a safeguard anyway
        if (text[i] < 32 || text[i] > 126)
            continue;

        float u = atlas_uvs[text[i] - 32][0];
        float v = atlas_uvs[text[i] - 32][1];
        vertex_data[cur_vertex + 0] = CUSTOMVERTEX{
            {x_offset + cur_xpos + padding + 8.0f, y_offset + padding * 2.0f + 12.0f, 0.5f},
            text_color,
            {u + char_width, v + char_height}
        };
        vertex_data[cur_vertex + 1] = CUSTOMVERTEX{
            {x_offset + cur_xpos + padding + 8.0f, y_offset + padding * 2.0f, 0.5},
            text_color,
            {u + char_width, v}
        };
        vertex_data[cur_vertex + 2] = CUSTOMVERTEX{
            {x_offset + cur_xpos + padding, y_offset + padding * 2.0f, 0.5},
            text_color,
            {u, v}
        };
        vertex_data[cur_vertex + 3] = CUSTOMVERTEX{
            {x_offset + cur_xpos + padding, y_offset + padding * 2.0f + 12.0f, 0.5},
            text_color,
            {u, v + char_height}
        };
        index_data[cur_index + 0] = (uint16_t)cur_vertex + 0;
        index_data[cur_index + 1] = (uint16_t)cur_vertex + 1;
        index_data[cur_index + 2] = (uint16_t)cur_vertex + 3;
        index_data[cur_index + 3] = (uint16_t)cur_vertex + 1;
        index_data[cur_index + 4] = (uint16_t)cur_vertex + 2;
        index_data[cur_index + 5] = (uint16_t)cur_vertex + 3;

        cur_vertex += 4;
        cur_index += 6;
        cur_xpos += 7.0f; // This is intentional, otherwise the text will be too spaced out
    }

    // Unlock the buffers
    d3d8_vertex_buf->Unlock();
    d3d8_index_buf->Unlock();

    return text_len + 1;
}

void D3D8Overlay::Draw() {
    // Save the current render state so the game's graphics don't get messed up
    DWORD original_state = 0;
    d3d8_device->CreateStateBlock(D3DSBT_ALL, &original_state);
    d3d8_device->CaptureStateBlock(original_state);

    // Load the desired text rendering render state
    d3d8_device->ApplyStateBlock(d3d8_state_block);

    // The game may have changed the viewport (the pause screen does), and state blocks don't
    // carry it along, so put ours back before drawing
    SetupViewportAndTransforms();

    // Draw shit
    auto rect_count = UpdateBuffers(text_buffer);

    d3d8_device->SetStreamSource(0, d3d8_vertex_buf, sizeof(CUSTOMVERTEX));
    d3d8_device->SetIndices(d3d8_index_buf, 0);
    d3d8_device->SetVertexShader(D3DFVF_CUSTOMVERTEX);
    d3d8_device->SetTexture(0, d3d8_font_tex);
    d3d8_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, rect_count * 2 * 3, 0, rect_count * 2);

    // Restore the original render state
    d3d8_device->ApplyStateBlock(original_state);
    d3d8_device->DeleteStateBlock(original_state);
}

void D3D8Overlay::SetText(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsprintf_s(text_buffer, format, args);
    va_end(args);
}

bool d3d8_overlay_active() {
	return D3D8Overlay::Instance != nullptr;
}

void d3d8_overlay_set_text(const char* text, unsigned long color) {
	if (D3D8Overlay::Instance == nullptr)
		return;
	D3D8Overlay::Instance->SetText("%s", text);
	D3D8Overlay::Instance->text_color = color;
}
