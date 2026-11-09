#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "overlay.h"
#include "x64_overlay.h"

namespace {
constexpr int kOverlayMargin = 8;
constexpr int kOverlayWidth = 230;

struct Overlay {
	HWND game_window;
	HDC memory_dc;
	HBITMAP bitmap;
	HGDIOBJ old_bitmap;
	void* pixels;
	int width;
	int height;
	HFONT font;
	DWORD last_refresh;
	char frame_time[96];
	bool visible;
	bool content_dirty;
};

struct OverlayRenderer {
	ID3D11Device* device;
	ID3D11DeviceContext* context;
	ID3D11VertexShader* vertex_shader;
	ID3D11PixelShader* pixel_shader;
	ID3D11InputLayout* input_layout;
	ID3D11Buffer* vertex_buffer;
	ID3D11Buffer* index_buffer;
	ID3D11Texture2D* texture;
	ID3D11ShaderResourceView* texture_view;
	ID3D11SamplerState* sampler;
	ID3D11BlendState* blend;
	ID3D11RasterizerState* rasterizer;
	int texture_width;
	int texture_height;
	bool texture_dirty;
};

struct OverlayVertex {
	float x;
	float y;
	float u;
	float v;
};

struct SavedD3D11State {
	ID3D11DeviceContext* context;
	ID3D11RenderTargetView* render_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* depth_stencil;
	ID3D11VertexShader* vertex_shader;
	ID3D11HullShader* hull_shader;
	ID3D11DomainShader* domain_shader;
	ID3D11GeometryShader* geometry_shader;
	ID3D11PixelShader* pixel_shader;
	ID3D11InputLayout* input_layout;
	D3D11_PRIMITIVE_TOPOLOGY primitive_topology;
	ID3D11Buffer* vertex_buffer;
	UINT vertex_stride;
	UINT vertex_offset;
	ID3D11Buffer* index_buffer;
	DXGI_FORMAT index_format;
	UINT index_offset;
	ID3D11ShaderResourceView* pixel_resource;
	ID3D11SamplerState* pixel_sampler;
	ID3D11BlendState* blend;
	FLOAT blend_factor[4];
	UINT sample_mask;
	ID3D11DepthStencilState* depth_stencil_state;
	UINT stencil_ref;
	ID3D11RasterizerState* rasterizer_state;
	UINT viewport_count;
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];

	explicit SavedD3D11State(ID3D11DeviceContext* value) : context(value) {
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
			render_targets, &depth_stencil);
		context->VSGetShader(&vertex_shader, nullptr, nullptr);
		context->HSGetShader(&hull_shader, nullptr, nullptr);
		context->DSGetShader(&domain_shader, nullptr, nullptr);
		context->GSGetShader(&geometry_shader, nullptr, nullptr);
		context->PSGetShader(&pixel_shader, nullptr, nullptr);
		context->IAGetInputLayout(&input_layout);
		context->IAGetPrimitiveTopology(&primitive_topology);
		context->IAGetVertexBuffers(0, 1, &vertex_buffer, &vertex_stride,
			&vertex_offset);
		context->IAGetIndexBuffer(&index_buffer, &index_format, &index_offset);
		context->PSGetShaderResources(0, 1, &pixel_resource);
		context->PSGetSamplers(0, 1, &pixel_sampler);
		context->OMGetBlendState(&blend, blend_factor, &sample_mask);
		context->OMGetDepthStencilState(&depth_stencil_state, &stencil_ref);
		context->RSGetState(&rasterizer_state);
		viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		context->RSGetViewports(&viewport_count, viewports);
	}

	~SavedD3D11State() {
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
			render_targets, depth_stencil);
		context->VSSetShader(vertex_shader, nullptr, 0);
		context->HSSetShader(hull_shader, nullptr, 0);
		context->DSSetShader(domain_shader, nullptr, 0);
		context->GSSetShader(geometry_shader, nullptr, 0);
		context->PSSetShader(pixel_shader, nullptr, 0);
		context->IASetInputLayout(input_layout);
		context->IASetPrimitiveTopology(primitive_topology);
		context->IASetVertexBuffers(0, 1, &vertex_buffer, &vertex_stride,
			&vertex_offset);
		context->IASetIndexBuffer(index_buffer, index_format, index_offset);
		context->PSSetShaderResources(0, 1, &pixel_resource);
		context->PSSetSamplers(0, 1, &pixel_sampler);
		context->OMSetBlendState(blend, blend_factor, sample_mask);
		context->OMSetDepthStencilState(depth_stencil_state, stencil_ref);
		context->RSSetState(rasterizer_state);
		context->RSSetViewports(viewport_count, viewports);
		release();
	}

	void release() {
		for (ID3D11RenderTargetView*& target : render_targets)
			if (target != nullptr) {
				target->Release();
				target = nullptr;
			}
		if (depth_stencil != nullptr) {
			depth_stencil->Release();
			depth_stencil = nullptr;
		}
		if (vertex_shader != nullptr) {
			vertex_shader->Release();
			vertex_shader = nullptr;
		}
		if (hull_shader != nullptr) {
			hull_shader->Release();
			hull_shader = nullptr;
		}
		if (domain_shader != nullptr) {
			domain_shader->Release();
			domain_shader = nullptr;
		}
		if (geometry_shader != nullptr) {
			geometry_shader->Release();
			geometry_shader = nullptr;
		}
		if (pixel_shader != nullptr) {
			pixel_shader->Release();
			pixel_shader = nullptr;
		}
		if (input_layout != nullptr) {
			input_layout->Release();
			input_layout = nullptr;
		}
		if (vertex_buffer != nullptr) {
			vertex_buffer->Release();
			vertex_buffer = nullptr;
		}
		if (index_buffer != nullptr) {
			index_buffer->Release();
			index_buffer = nullptr;
		}
		if (pixel_resource != nullptr) {
			pixel_resource->Release();
			pixel_resource = nullptr;
		}
		if (pixel_sampler != nullptr) {
			pixel_sampler->Release();
			pixel_sampler = nullptr;
		}
		if (blend != nullptr) {
			blend->Release();
			blend = nullptr;
		}
		if (depth_stencil_state != nullptr) {
			depth_stencil_state->Release();
			depth_stencil_state = nullptr;
		}
		if (rasterizer_state != nullptr) {
			rasterizer_state->Release();
			rasterizer_state = nullptr;
		}
	}
};

Overlay overlay;
OverlayRenderer renderer;
SRWLOCK overlay_lock = SRWLOCK_INIT;
bool present_rate_enabled;
double present_rate;
unsigned int present_count;
LARGE_INTEGER present_rate_frequency;
LARGE_INTEGER present_rate_since;

class OverlayLock {
public:
	OverlayLock() {
		AcquireSRWLockExclusive(&overlay_lock);
	}
	~OverlayLock() {
		ReleaseSRWLockExclusive(&overlay_lock);
	}
};

template <typename T>
void release_com(T*& object) {
	T* released = object;
	object = nullptr;
	if (released != nullptr)
		released->Release();
}

const char shader_source[] = R"(
struct VSInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput vs_main(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

Texture2D overlay_texture : register(t0);
SamplerState overlay_sampler : register(s0);

float4 ps_main(VSOutput input) : SV_TARGET {
    return overlay_texture.Sample(overlay_sampler, input.uv);
}
)";

void release_renderer() {
	release_com(renderer.vertex_shader);
	release_com(renderer.pixel_shader);
	release_com(renderer.input_layout);
	release_com(renderer.vertex_buffer);
	release_com(renderer.index_buffer);
	release_com(renderer.texture_view);
	release_com(renderer.texture);
	release_com(renderer.sampler);
	release_com(renderer.blend);
	release_com(renderer.rasterizer);
	release_com(renderer.context);
	release_com(renderer.device);
	renderer = {};
}

bool ensure_gdi_resources(int width, int height) {
	if (overlay.memory_dc == nullptr)
		overlay.memory_dc = CreateCompatibleDC(nullptr);
	if (overlay.font == nullptr) {
		overlay.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
	}
	if (overlay.memory_dc == nullptr || overlay.font == nullptr)
		return false;
	if (overlay.bitmap != nullptr && overlay.width == width && overlay.height == height)
		return true;

	if (overlay.old_bitmap != nullptr) {
		SelectObject(overlay.memory_dc, overlay.old_bitmap);
		overlay.old_bitmap = nullptr;
	}
	if (overlay.bitmap != nullptr)
		DeleteObject(overlay.bitmap);

	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	overlay.bitmap = CreateDIBSection(overlay.memory_dc, &info, DIB_RGB_COLORS,
		&overlay.pixels, nullptr, 0);
	if (overlay.bitmap == nullptr)
		return false;
	overlay.old_bitmap = SelectObject(overlay.memory_dc, overlay.bitmap);
	overlay.width = width;
	overlay.height = height;
	return true;
}

bool overlay_is_foreground() {
	return overlay.game_window != nullptr && IsWindow(overlay.game_window) &&
		IsWindowVisible(overlay.game_window) &&
		GetForegroundWindow() == GetAncestor(overlay.game_window, GA_ROOT);
}

void update_content(bool force) {
	if (Config::ShowOverlay == 0 || !overlay_is_foreground()) {
		overlay.visible = false;
		return;
	}

	const DWORD now = GetTickCount();
	if (!force && !overlay.content_dirty && now - overlay.last_refresh < 200)
		return;
	overlay.last_refresh = now;
	overlay.content_dirty = false;

	char lines[2][96] = {};
	int line_count = 0;
	if ((Config::ShowOverlay & 1) != 0 && present_rate_enabled && present_rate > 0.0) {
		sprintf_s(lines[line_count++], "present %.1f fps", present_rate);
	}
	if ((Config::ShowOverlay & 2) != 0 && overlay.frame_time[0] != '\0')
		strcpy_s(lines[line_count++], overlay.frame_time);
	if (line_count == 0) {
		overlay.visible = false;
		return;
	}

	const int height = 22 * line_count + 8;
	if (!ensure_gdi_resources(kOverlayWidth, height)) {
		overlay.visible = false;
		return;
	}

	RECT fill = { 0, 0, overlay.width, overlay.height };
	HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
	FillRect(overlay.memory_dc, &fill, background);
	DeleteObject(background);
	SetBkMode(overlay.memory_dc, TRANSPARENT);
	HGDIOBJ old_font = SelectObject(overlay.memory_dc, overlay.font);
	SetTextColor(overlay.memory_dc, RGB(235, 235, 235));
	for (int i = 0; i < line_count; ++i)
		TextOutA(overlay.memory_dc, 6, 4 + i * 22, lines[i], (int)strlen(lines[i]));
	SelectObject(overlay.memory_dc, old_font);

	uint32_t* pixels = (uint32_t*)overlay.pixels;
	for (int i = 0; i < overlay.width * overlay.height; ++i)
		pixels[i] = (pixels[i] & 0x00FFFFFFu) | 0xC0000000u;

	overlay.visible = true;
	renderer.texture_dirty = true;
}

bool ensure_renderer(IDXGISwapChain* swapchain) {
	ID3D11Device* device = nullptr;
	if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), (void**)&device)))
		return false;

	if (renderer.device != device) {
		release_renderer();
		renderer.device = device;
		renderer.device->AddRef();
		renderer.device->GetImmediateContext(&renderer.context);
	}
	device->Release();
	if (renderer.context == nullptr)
		return false;
	if (renderer.vertex_shader != nullptr && renderer.pixel_shader != nullptr &&
		renderer.input_layout != nullptr && renderer.vertex_buffer != nullptr &&
		renderer.index_buffer != nullptr && renderer.sampler != nullptr &&
		renderer.blend != nullptr && renderer.rasterizer != nullptr)
		return true;
	if (renderer.vertex_shader != nullptr)
		release_renderer();
	if (renderer.device == nullptr) {
		if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), (void**)&device)))
			return false;
		renderer.device = device;
		renderer.device->AddRef();
		renderer.device->GetImmediateContext(&renderer.context);
		device->Release();
		if (renderer.context == nullptr)
			return false;
	}

	ID3DBlob* vertex_blob = nullptr;
	ID3DBlob* pixel_blob = nullptr;
	ID3DBlob* errors = nullptr;
	const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
	HRESULT result = D3DCompile(shader_source, sizeof(shader_source) - 1,
		nullptr, nullptr, nullptr, "vs_main", "vs_4_0", flags, 0,
		&vertex_blob, &errors);
	if (FAILED(result)) {
		printf("D3D11 overlay vertex shader failed: %s\n",
			errors != nullptr ? (const char*)errors->GetBufferPointer() : "unknown");
		if (errors != nullptr)
			errors->Release();
		return false;
	}
	if (errors != nullptr) {
		errors->Release();
		errors = nullptr;
	}
	result = D3DCompile(shader_source, sizeof(shader_source) - 1,
		nullptr, nullptr, nullptr, "ps_main", "ps_4_0", flags, 0,
		&pixel_blob, &errors);
	if (FAILED(result)) {
		printf("D3D11 overlay pixel shader failed: %s\n",
			errors != nullptr ? (const char*)errors->GetBufferPointer() : "unknown");
		if (errors != nullptr)
			errors->Release();
		vertex_blob->Release();
		return false;
	}
	if (errors != nullptr)
		errors->Release();

	result = renderer.device->CreateVertexShader(vertex_blob->GetBufferPointer(),
		vertex_blob->GetBufferSize(), nullptr, &renderer.vertex_shader);
	if (SUCCEEDED(result)) {
		const D3D11_INPUT_ELEMENT_DESC elements[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		result = renderer.device->CreateInputLayout(elements, _countof(elements),
			vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
			&renderer.input_layout);
	}
	vertex_blob->Release();
	if (SUCCEEDED(result)) {
		result = renderer.device->CreatePixelShader(pixel_blob->GetBufferPointer(),
			pixel_blob->GetBufferSize(), nullptr, &renderer.pixel_shader);
	}
	pixel_blob->Release();
	if (FAILED(result))
		return false;

	D3D11_BUFFER_DESC buffer = {};
	buffer.ByteWidth = sizeof(OverlayVertex) * 4;
	buffer.Usage = D3D11_USAGE_DYNAMIC;
	buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(renderer.device->CreateBuffer(&buffer, nullptr,
			&renderer.vertex_buffer)))
		return false;

	const uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
	D3D11_SUBRESOURCE_DATA index_data = {};
	index_data.pSysMem = indices;
	buffer.ByteWidth = sizeof(indices);
	buffer.Usage = D3D11_USAGE_IMMUTABLE;
	buffer.BindFlags = D3D11_BIND_INDEX_BUFFER;
	buffer.CPUAccessFlags = 0;
	if (FAILED(renderer.device->CreateBuffer(&buffer, &index_data,
			&renderer.index_buffer)))
		return false;

	D3D11_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(renderer.device->CreateSamplerState(&sampler, &renderer.sampler)))
		return false;

	D3D11_BLEND_DESC blend = {};
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(renderer.device->CreateBlendState(&blend, &renderer.blend)))
		return false;

	D3D11_RASTERIZER_DESC rasterizer = {};
	rasterizer.FillMode = D3D11_FILL_SOLID;
	rasterizer.CullMode = D3D11_CULL_NONE;
	rasterizer.DepthClipEnable = TRUE;
	rasterizer.ScissorEnable = FALSE;
	return SUCCEEDED(renderer.device->CreateRasterizerState(&rasterizer,
		&renderer.rasterizer));
}

bool ensure_texture() {
	if (renderer.texture != nullptr && renderer.texture_width == overlay.width &&
		renderer.texture_height == overlay.height)
		return true;

	if (renderer.texture_view != nullptr) {
		renderer.texture_view->Release();
		renderer.texture_view = nullptr;
	}
	if (renderer.texture != nullptr) {
		renderer.texture->Release();
		renderer.texture = nullptr;
	}

	D3D11_TEXTURE2D_DESC texture = {};
	texture.Width = overlay.width;
	texture.Height = overlay.height;
	texture.MipLevels = 1;
	texture.ArraySize = 1;
	texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	texture.SampleDesc.Count = 1;
	texture.Usage = D3D11_USAGE_DYNAMIC;
	texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	texture.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(renderer.device->CreateTexture2D(&texture, nullptr,
			&renderer.texture)))
		return false;

	D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
	view.Format = texture.Format;
	view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	view.Texture2D.MipLevels = 1;
	if (FAILED(renderer.device->CreateShaderResourceView(renderer.texture, &view,
			&renderer.texture_view)))
		return false;
	renderer.texture_width = overlay.width;
	renderer.texture_height = overlay.height;
	renderer.texture_dirty = true;
	return true;
}

bool upload_texture() {
	if (!renderer.texture_dirty || renderer.texture == nullptr)
		return true;
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(renderer.context->Map(renderer.texture, 0, D3D11_MAP_WRITE_DISCARD,
			0, &mapped)))
		return false;
	for (int y = 0; y < overlay.height; ++y) {
		memcpy((uint8_t*)mapped.pData + mapped.RowPitch * y,
			(uint8_t*)overlay.pixels + overlay.width * 4 * y,
			overlay.width * 4);
	}
	renderer.context->Unmap(renderer.texture, 0);
	renderer.texture_dirty = false;
	return true;
}

void draw_overlay(IDXGISwapChain* swapchain) {
	DXGI_SWAP_CHAIN_DESC swap_desc = {};
	if (FAILED(swapchain->GetDesc(&swap_desc)) ||
		swap_desc.BufferDesc.Width == 0 || swap_desc.BufferDesc.Height == 0)
		return;

	ID3D11Texture2D* back_buffer = nullptr;
	if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
		return;
	ID3D11RenderTargetView* render_target = nullptr;
	const HRESULT create_result =
		renderer.device->CreateRenderTargetView(back_buffer, nullptr, &render_target);
	back_buffer->Release();
	if (FAILED(create_result))
		return;

	SavedD3D11State saved(renderer.context);
	renderer.context->OMSetRenderTargets(1, &render_target, nullptr);
	renderer.context->OMSetDepthStencilState(nullptr, 0);
	renderer.context->RSSetState(renderer.rasterizer);
	const float blend_factor[4] = {};
	renderer.context->OMSetBlendState(renderer.blend, blend_factor, 0xFFFFFFFFu);
	renderer.context->IASetInputLayout(renderer.input_layout);
	renderer.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	renderer.context->HSSetShader(nullptr, nullptr, 0);
	renderer.context->DSSetShader(nullptr, nullptr, 0);
	renderer.context->GSSetShader(nullptr, nullptr, 0);

	const float width = (float)swap_desc.BufferDesc.Width;
	const float height = (float)swap_desc.BufferDesc.Height;
	const float left = (float)kOverlayMargin;
	const float right = left + (float)overlay.width;
	const float bottom = height - (float)kOverlayMargin;
	const float top = bottom - (float)overlay.height;
	const OverlayVertex vertices[] = {
		{ left / width * 2.0f - 1.0f, 1.0f - top / height * 2.0f, 0.0f, 0.0f },
		{ right / width * 2.0f - 1.0f, 1.0f - top / height * 2.0f, 1.0f, 0.0f },
		{ right / width * 2.0f - 1.0f, 1.0f - bottom / height * 2.0f, 1.0f, 1.0f },
		{ left / width * 2.0f - 1.0f, 1.0f - bottom / height * 2.0f, 0.0f, 1.0f },
	};
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(renderer.context->Map(renderer.vertex_buffer, 0,
			D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		memcpy(mapped.pData, vertices, sizeof(vertices));
		renderer.context->Unmap(renderer.vertex_buffer, 0);

		const UINT stride = sizeof(OverlayVertex);
		const UINT offset = 0;
		renderer.context->IASetVertexBuffers(0, 1, &renderer.vertex_buffer,
			&stride, &offset);
		renderer.context->IASetIndexBuffer(renderer.index_buffer,
			DXGI_FORMAT_R16_UINT, 0);
		renderer.context->VSSetShader(renderer.vertex_shader, nullptr, 0);
		renderer.context->PSSetShader(renderer.pixel_shader, nullptr, 0);
		renderer.context->PSSetShaderResources(0, 1, &renderer.texture_view);
		renderer.context->PSSetSamplers(0, 1, &renderer.sampler);
		const D3D11_VIEWPORT viewport = {
			0.0f, 0.0f, width, height, 0.0f, 1.0f
		};
		renderer.context->RSSetViewports(1, &viewport);
		renderer.context->DrawIndexed(6, 0, 0);
	}

	render_target->Release();
}
}

void overlay_show_present_rate(bool show) {
	OverlayLock lock;
	present_rate_enabled = show;
}

bool overlay_present_rate_enabled() {
	return present_rate_enabled;
}

double overlay_present_rate() {
	return present_rate;
}

void overlay_set_present_rate_anchor(int, int) {
}

void overlay_set_present_rate_follow_text(bool) {
}

void overlay_set_render_time_text(const char* text, unsigned long) {
	OverlayLock lock;
	strncpy_s(overlay.frame_time, text != nullptr ? text : "", _TRUNCATE);
	overlay.content_dirty = true;
}

void overlay_mark_presentation() {
	OverlayLock lock;
	if (present_rate_frequency.QuadPart == 0)
		QueryPerformanceFrequency(&present_rate_frequency);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (present_rate_since.QuadPart == 0) {
		present_rate_since = now;
		present_count = 0;
		return;
	}
	++present_count;
	const double seconds = (double)(now.QuadPart - present_rate_since.QuadPart) /
		(double)present_rate_frequency.QuadPart;
	if (seconds >= 0.5) {
		const double measured = (double)present_count / seconds;
		present_rate = present_rate <= 0.0 ? measured : present_rate * 0.5 + measured * 0.5;
		present_rate_since = now;
		present_count = 0;
	}
}

void x64_overlay_attach(HWND game_window) {
	OverlayLock lock;
	if (overlay.game_window == game_window)
		return;
	overlay.game_window = game_window;
	overlay.content_dirty = true;
}

void x64_overlay_draw(IDXGISwapChain* swapchain) {
	if (swapchain == nullptr)
		return;
	OverlayLock lock;
	update_content(false);
	if (!overlay.visible || !ensure_renderer(swapchain) || !ensure_texture() ||
		!upload_texture())
		return;
	draw_overlay(swapchain);
}

void x64_overlay_destroy() {
	OverlayLock lock;
	release_renderer();
	if (overlay.old_bitmap != nullptr)
		SelectObject(overlay.memory_dc, overlay.old_bitmap);
	if (overlay.bitmap != nullptr)
		DeleteObject(overlay.bitmap);
	if (overlay.font != nullptr)
		DeleteObject(overlay.font);
	if (overlay.memory_dc != nullptr)
		DeleteDC(overlay.memory_dc);
	overlay = {};
}
