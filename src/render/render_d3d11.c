// render_d3d11.c - instanced-quad 2D renderer.
//
// Everything on screen is a rect instance: dst rect, atlas uv rect, two colors, params (corner
// radius, border thickness, edge softness, texture) and a shape. The pixel shader evaluates the
// shape's signed distance (a rounded rect, a cog wheel, a diamond, a star, a crescent, a leaf or a
// tapered stroke), so rounded corners, borders and soft shadows are all free, and blends the two colors along the rect (a gradient). Rects are
// grouped into batches that share a clip rect. One instance buffer, one draw call per batch.
//
// Three textures: the glyph atlas (8-bit coverage, point-sampled), the image
// atlas (BGRA with straight alpha, linear-sampled) for small pictures such as
// icons, and one full-size picture (sized to it, e.g. a screenshot being viewed).
// Picture rects are still clipped by the SDF, so they get rounded corners.

typedef struct R_Rect {
    F32 dst[4];
    F32 src[4];
    F32 color[4];
    F32 params[4]; // radius, border, softness, texture (0 none, 1 glyph atlas, 2 image atlas, 3 the picture)
    F32 color2[4]; // the gradient's end color (= color for a solid fill)
    F32 shape[4];  // R_SHAPE_*, then: rect: 1 = the gradient runs left to right (else top to bottom);
                   // cog: teeth, root radius / outer radius, rotation (radians)
                   // star: points, sharpness (2 = spiky .. points = a polygon), rotation
                   // crescent: cut radius / radius, cut offset / radius, rotation (0: opens to the right)
                   // leaf: half width, half height, rotation (0: upright)
                   // taper: radius at a, radius at b (a, b in src, relative to dst's center)
} R_Rect;

enum { R_SHAPE_RECT, R_SHAPE_COG, R_SHAPE_DIAMOND, R_SHAPE_STAR, R_SHAPE_CRESCENT, R_SHAPE_LEAF, R_SHAPE_TAPER };

typedef struct R_Batch { U32 first, count; Rect clip; } R_Batch;

#define R_MAX_RECTS   (1 << 16)
#define R_MAX_BATCHES 1024

typedef struct R_State {
    ID3D11Device *device;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain1 *swapchain;
    ID3D11RenderTargetView *rtv;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *instances;
    ID3D11Buffer *globals;
    ID3D11BlendState *blend;
    ID3D11RasterizerState *raster;
    ID3D11SamplerState *sampler, *sampler_linear;
    ID3D11Texture2D *atlas;
    ID3D11ShaderResourceView *atlas_srv;
    U32 atlas_w, atlas_h;
    ID3D11Texture2D *images;
    ID3D11ShaderResourceView *images_srv;
    U32 images_w, images_h;
    ID3D11Texture2D *picture;
    ID3D11ShaderResourceView *picture_srv;
    U32 picture_w, picture_h;
    U32 width, height;   // what is drawn: the window's client size
    U64 us_resize, us_present;   // how long the last resize and present took (profiling)
    R_Rect rects[R_MAX_RECTS];
    U32 rect_count;
    R_Batch batches[R_MAX_BATCHES];
    U32 batch_count;
} R_State;

static R_State *r_state;

static char r_shader_src[] =
"cbuffer Globals : register(b0) { float2 viewport; float2 pad; };\n"
"struct VSIn { float4 dst : DST; float4 src : SRC; float4 col : COL; float4 params : PARAMS; float4 col2 : COLB; float4 shape : SHAPE;\n"
"              uint vid : SV_VertexID; };\n"
"struct PSIn { float4 pos : SV_Position; float2 uv : UV; float2 local : LOCAL; float2 half_size : HSIZE;\n"
"              float4 col : COL; float4 params : PARAMS; float4 col2 : COLB; float4 shape : SHAPE; float4 src : SRCC; };\n"
"PSIn vs_main(VSIn i) {\n"
"  float2 corner = float2((i.vid & 1) ? 1.0 : 0.0, (i.vid & 2) ? 1.0 : 0.0);\n"
"  float2 p = lerp(i.dst.xy, i.dst.zw, corner);\n"
"  PSIn o;\n"
"  o.pos = float4(p / viewport * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"  o.uv = lerp(i.src.xy, i.src.zw, corner);\n"
"  o.half_size = (i.dst.zw - i.dst.xy) * 0.5;\n"
"  o.local = (corner * 2 - 1) * o.half_size;\n"
"  o.col = i.col; o.params = i.params; o.col2 = i.col2; o.shape = i.shape; o.src = i.src;\n"
"  return o;\n"
"}\n"
"Texture2D atlas : register(t0); SamplerState samp : register(s0);\n"
"Texture2D images : register(t1); SamplerState samp_lin : register(s1);\n"
"Texture2D picture : register(t2);\n"
"float rrect(float2 p, float2 b, float r) { float2 q = abs(p) - b + r; return length(max(q, 0)) + min(max(q.x, q.y), 0) - r; }\n"
"float diamond(float2 p, float2 b) { float2 q = abs(p) / b; return (q.x + q.y - 1) * min(b.x, b.y) * 0.7071; }\n"
// A cog: a disc of the root radius plus teeth, each tooth a sector narrowing from root to tip.
"float cog(float2 p, float ro, float teeth, float ratio, float rot) {\n"
"  float ri = ro * ratio, r = length(p), seg = 6.2831853 / teeth;\n"
"  float f = abs(frac((atan2(p.y, p.x) + rot) / seg) - 0.5) * seg;\n"
"  float hw = lerp(seg * 0.30, seg * 0.17, saturate((r - ri) / max(ro - ri, 0.001)));\n"
"  return min(r - ri, max(r - ro, (f - hw) * r));\n"
"}\n"
"float2 rot(float2 p, float a) { float c = cos(a), s = sin(a); return float2(c * p.x + s * p.y, -s * p.x + c * p.y); }\n"
// A star of n points (after Inigo Quilez): m from 2 (spiky) to n (a regular polygon). A point is up.
"float star(float2 p, float r, float n, float m) {\n"
"  float an = 3.141593 / n, en = 3.141593 / m;\n"
"  float2 acs = float2(cos(an), sin(an)), ecs = float2(cos(en), sin(en));\n"
"  float a = atan2(p.x, -p.y), w = 2 * an, bn = a - w * floor(a / w) - an;\n"
"  p = length(p) * float2(cos(bn), abs(sin(bn)));\n"
"  p -= r * acs;\n"
"  p += ecs * clamp(-dot(p, ecs), 0.0, r * acs.y / ecs.y);\n"
"  return length(p) * sign(p.x);\n"
"}\n"
// Two arcs meeting in points at the top and bottom (a vesica): w half wide, h half tall (w < h).
"float leaf(float2 p, float w, float h) {\n"
"  p = abs(p);\n"
"  float d = (h * h / w - w) * 0.5, r = w + d;\n"
"  return ((p.y - h) * d > p.x * h) ? length(p - float2(0, h)) : length(p + float2(d, 0)) - r;\n"
"}\n"
// A stroke from a (radius ra) to b (radius rb): Inigo Quilez's uneven capsule.
"float taper(float2 p, float2 pa, float2 pb, float ra, float rb) {\n"
"  p -= pa; pb -= pa;\n"
"  float h = max(dot(pb, pb), 0.0001);\n"
"  float2 q = float2(dot(p, float2(pb.y, -pb.x)), dot(p, pb)) / h;\n"
"  q.x = abs(q.x);\n"
"  float b = ra - rb;\n"
"  float2 c = float2(sqrt(max(h - b * b, 0.0001)), b);\n"
"  float k = c.x * q.y - c.y * q.x, m = dot(c, q), n = dot(q, q);\n"
"  if (k < 0.0) return sqrt(h * n) - ra;\n"
"  if (k > c.x) return sqrt(h * (n + 1.0 - 2.0 * q.y)) - rb;\n"
"  return m - ra;\n"
"}\n"
"float4 ps_main(PSIn i) : SV_Target {\n"
"  float radius = i.params.x, border = i.params.y, soft = i.params.z, kind = i.shape.x;\n"
"  float g = (kind < 0.5 && i.shape.y > 0.5) ? i.local.x / max(i.half_size.x, 0.001) : i.local.y / max(i.half_size.y, 0.001);\n"
"  float4 c = lerp(i.col, i.col2, saturate(g * 0.5 + 0.5));\n"
"  if (i.params.w > 2.5) c *= picture.Sample(samp_lin, i.uv);\n"
"  else if (i.params.w > 1.5) c *= images.Sample(samp_lin, i.uv);\n"
"  else if (i.params.w > 0.5) c.a *= atlas.Sample(samp, i.uv).r;\n"
"  if (soft > 0) {\n"
"    float d;\n"
"    float rr = min(i.half_size.x, i.half_size.y);\n"
"    if (kind > 5.5) d = taper(i.local, i.src.xy, i.src.zw, i.shape.y, i.shape.z);\n"
"    else if (kind > 4.5) d = leaf(rot(i.local, i.shape.w), i.shape.y, i.shape.z);\n"
"    else if (kind > 3.5) d = max(length(i.local) - rr, i.shape.y * rr - length(i.local - float2(cos(i.shape.w), sin(i.shape.w)) * i.shape.z * rr));\n"
"    else if (kind > 2.5) d = star(rot(i.local, i.shape.w), rr, i.shape.y, i.shape.z);\n"
"    else if (kind > 1.5) d = diamond(i.local, i.half_size);\n"
"    else if (kind > 0.5) d = max(cog(i.local, min(i.half_size.x, i.half_size.y), i.shape.y, i.shape.z, i.shape.w), radius - length(i.local));\n"
"    else d = rrect(i.local, i.half_size, radius);\n"
"    c.a *= 1 - smoothstep(-soft, soft, d);\n"
"    if (border > 0) {\n"
"      float inner = kind > 0.5 ? d + border : rrect(i.local, i.half_size - border, max(radius - border, 0));\n"
"      c.a *= smoothstep(-soft, soft, inner);\n"
"    }\n"
"  }\n"
"  return c;\n"
"}\n";

static void r_create_rtv(void)
{
    ID3D11Texture2D *bb = 0;
    IDXGISwapChain1_GetBuffer(r_state->swapchain, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(r_state->device, (ID3D11Resource *)bb, 0, &r_state->rtv);
    ID3D11Texture2D_Release(bb);
}

static ID3DBlob *r_compile(char *entry, char *target)
{
    ID3DBlob *blob = 0, *err = 0;
    HRESULT hr = D3DCompile(r_shader_src, sizeof(r_shader_src) - 1, "ui.hlsl", 0, 0, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) OutputDebugStringA((char *)ID3D10Blob_GetBufferPointer(err));
        MessageBoxA(0, err ? (char *)ID3D10Blob_GetBufferPointer(err) : "shader compile failed", "wam", MB_OK);
        ExitProcess(1);
    }
    if (err) ID3D10Blob_Release(err);
    return blob;
}

static void r_resize_buffers(U32 w, U32 h);

#define R_IMAGES_SIZE 2048 // the image atlas, BGRA

// window: the native window to draw into (wm_window_handle(): an HWND).
static B32 r_init(void *window, U32 atlas_w, U32 atlas_h)
{
    HWND hwnd = (HWND)window;
    r_state = (R_State *)VirtualAlloc(0, sizeof(R_State), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    R_State *r = r_state;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, flags, levels, 1, D3D11_SDK_VERSION, &r->device, 0, &r->ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_WARP, 0, flags, levels, 1, D3D11_SDK_VERSION, &r->device, 0, &r->ctx);
    if (FAILED(hr)) return 0;

    IDXGIDevice *dxgi_device = 0;
    IDXGIAdapter *adapter = 0;
    IDXGIFactory2 *factory = 0;
    ID3D11Device_QueryInterface(r->device, &IID_IDXGIDevice, (void **)&dxgi_device);
    IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);

    DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // The bitblt ("discard") model, not flip, on purpose. While a window edge is dragged, the
    // compositor holds each new window size back until the app has presented a frame of that size.
    // With flip-model swapchains that handshake costs about two refreshes per step (measured with
    // real drags: 66-82 fps at 144 Hz, whatever the present interval, latency or buffer count) and
    // the content trails the edge. With the bitblt model it presents into the window's redirection
    // surface and follows the edge at the full refresh rate (137-139 fps), and ResizeBuffers is
    // 0.2 ms instead of 1-16 ms. The cost, about 1 ms per present for the copy, doesn't show.
    // (The window must not use WS_EX_NOREDIRECTIONBITMAP for this.)
    sd.BufferCount = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)r->device, hwnd, &sd, 0, 0, &r->swapchain);
    IDXGIFactory2_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGIFactory2_Release(factory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_device);
    if (FAILED(hr)) return 0;

    DXGI_SWAP_CHAIN_DESC1 actual;
    IDXGISwapChain1_GetDesc1(r->swapchain, &actual);
    r->width = actual.Width;
    r->height = actual.Height;
    r_create_rtv();

    ID3DBlob *vs = r_compile("vs_main", "vs_5_0");
    ID3DBlob *ps = r_compile("ps_main", "ps_5_0");
    ID3D11Device_CreateVertexShader(r->device, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), 0, &r->vs);
    ID3D11Device_CreatePixelShader(r->device, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), 0, &r->ps);

    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "DST",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "SRC",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "COL",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "PARAMS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "COLB",   0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "SHAPE",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    ID3D11Device_CreateInputLayout(r->device, il, ArrayCount(il), ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), &r->layout);
    ID3D10Blob_Release(vs);
    ID3D10Blob_Release(ps);

    D3D11_BUFFER_DESC bd = { 0 };
    bd.ByteWidth = sizeof(R_Rect) * R_MAX_RECTS;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(r->device, &bd, 0, &r->instances);

    D3D11_BUFFER_DESC cd = { 0 };
    cd.ByteWidth = 16;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(r->device, &cd, 0, &r->globals);

    D3D11_BLEND_DESC blend = { 0 };
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(r->device, &blend, &r->blend);

    D3D11_RASTERIZER_DESC rd = { 0 };
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(r->device, &rd, &r->raster);

    D3D11_SAMPLER_DESC smp = { 0 };
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11Device_CreateSamplerState(r->device, &smp, &r->sampler);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(r->device, &smp, &r->sampler_linear);

    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = atlas_w;
    td.Height = atlas_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device_CreateTexture2D(r->device, &td, 0, &r->atlas);
    ID3D11Device_CreateShaderResourceView(r->device, (ID3D11Resource *)r->atlas, 0, &r->atlas_srv);
    r->atlas_w = atlas_w;
    r->atlas_h = atlas_h;

    td.Width = td.Height = R_IMAGES_SIZE;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D11Device_CreateTexture2D(r->device, &td, 0, &r->images);
    if (r->images) ID3D11Device_CreateShaderResourceView(r->device, (ID3D11Resource *)r->images, 0, &r->images_srv);
    r->images_w = r->images_h = R_IMAGES_SIZE;
    return 1;
}

// BGRA pixels (straight alpha) into the image atlas.
static void r_images_update(U32 x, U32 y, U32 w, U32 h, U8 *pixels, U32 pitch)
{
    if (!r_state->images) return;
    D3D11_BOX box = { x, y, 0, x + w, y + h, 1 };
    ID3D11DeviceContext_UpdateSubresource(r_state->ctx, (ID3D11Resource *)r_state->images, 0, &box, pixels, pitch, 0);
}

static void r_atlas_update(U32 x, U32 y, U32 w, U32 h, U8 *pixels, U32 pitch)
{
    D3D11_BOX box = { x, y, 0, x + w, y + h, 1 };
    ID3D11DeviceContext_UpdateSubresource(r_state->ctx, (ID3D11Resource *)r_state->atlas, 0, &box, pixels, pitch, 0);
}

// The buffers always match the window exactly. While an edge is dragged, the compositor holds
// the new window size back until the app presents a frame of that size; buffers of any other size
// (tried: monitor-sized buffers, to skip ResizeBuffers) never match, so every step waited for its
// timeout, about 4 refreshes.
static void r_resize_buffers(U32 w, U32 h)
{
    R_State *r = r_state;
    if (r->rtv) { ID3D11DeviceContext_OMSetRenderTargets(r->ctx, 0, 0, 0); ID3D11RenderTargetView_Release(r->rtv); r->rtv = 0; }
    IDXGISwapChain1_ResizeBuffers(r->swapchain, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    r_create_rtv();
}

static void r_resize(U32 w, U32 h)
{
    R_State *r = r_state;
    if (w == 0 || h == 0 || (w == r->width && h == r->height)) return;
    U64 t0 = os_now_us();
    r_resize_buffers(w, h);
    r->width = w;
    r->height = h;
    r->us_resize = os_now_us() - t0;
}

static void r_set_clip(Rect clip)
{
    R_State *r = r_state;
    R_Batch *last = r->batch_count ? &r->batches[r->batch_count - 1] : 0;
    if (last && last->count == 0) { last->clip = clip; return; }
    if (r->batch_count >= R_MAX_BATCHES) return;
    R_Batch *b = &r->batches[r->batch_count++];
    b->first = r->rect_count;
    b->count = 0;
    b->clip = clip;
}

static void r_begin_frame(void)
{
    r_state->rect_count = 0;
    r_state->batch_count = 0;
    Rect full = { 0, 0, (F32)r_state->width, (F32)r_state->height };
    r_set_clip(full);
}

static R_Rect *r_push(void)
{
    R_State *r = r_state;
    if (r->rect_count >= R_MAX_RECTS) { static R_Rect dummy; return &dummy; }
    r->batches[r->batch_count - 1].count += 1;
    R_Rect *rect = &r->rects[r->rect_count++];
    memset(rect, 0, sizeof(*rect));
    return rect;
}

static void r_set_colors(R_Rect *r, V4 a, V4 b)
{
    r->color[0] = a.x; r->color[1] = a.y; r->color[2] = a.z; r->color[3] = a.w;
    r->color2[0] = b.x; r->color2[1] = b.y; r->color2[2] = b.z; r->color2[3] = b.w;
}

// Solid / rounded / bordered rect. softness 0.5 = 1px antialiased edge; larger = blur (shadows).
static void r_rect(Rect dst, V4 color, F32 radius, F32 border, F32 softness)
{
    R_Rect *r = r_push();
    r->dst[0] = dst.x0; r->dst[1] = dst.y0; r->dst[2] = dst.x1; r->dst[3] = dst.y1;
    r_set_colors(r, color, color);
    r->params[0] = radius; r->params[1] = border; r->params[2] = softness;
}

// The same with a gradient: from a at the top to b at the bottom (horizontal: left to right).
static void r_rect_gradient(Rect dst, V4 a, V4 b, F32 radius, F32 border, F32 softness, B32 horizontal)
{
    R_Rect *r = r_push();
    r->dst[0] = dst.x0; r->dst[1] = dst.y0; r->dst[2] = dst.x1; r->dst[3] = dst.y1;
    r_set_colors(r, a, b);
    r->params[0] = radius; r->params[1] = border; r->params[2] = softness;
    r->shape[1] = horizontal ? 1.0f : 0.0f;
}

// A cog wheel in the circle inscribed in dst, colored top to bottom: `teeth` teeth, its root circle
// at inner_ratio of the radius, turned by angle (radians), with a hole of hole_radius (0: none).
// border > 0 draws only its outline, that thick.
static void r_cog(Rect dst, V4 top, V4 bottom, F32 teeth, F32 inner_ratio, F32 angle, F32 hole_radius, F32 border)
{
    R_Rect *r = r_push();
    r->dst[0] = dst.x0; r->dst[1] = dst.y0; r->dst[2] = dst.x1; r->dst[3] = dst.y1;
    r_set_colors(r, top, bottom);
    r->params[0] = hole_radius; r->params[1] = border; r->params[2] = 0.5f;
    r->shape[0] = R_SHAPE_COG; r->shape[1] = teeth; r->shape[2] = inner_ratio; r->shape[3] = angle;
}

// A star of `points` points in the circle inscribed in dst, one pointing up when angle is 0 (radians,
// clockwise), colored top to bottom. sharpness: 2 is spiky, up to `points` (a regular polygon).
static void r_star(Rect dst, V4 top, V4 bottom, F32 points, F32 sharpness, F32 angle, F32 border)
{
    R_Rect *r = r_push();
    r->dst[0] = dst.x0; r->dst[1] = dst.y0; r->dst[2] = dst.x1; r->dst[3] = dst.y1;
    r_set_colors(r, top, bottom);
    r->params[1] = border; r->params[2] = 0.5f;
    r->shape[0] = R_SHAPE_STAR; r->shape[1] = points; r->shape[2] = sharpness; r->shape[3] = angle;
}

// A crescent: the circle inscribed in dst less a circle cut * its radius, offset * its radius away
// in the direction angle (0: right, clockwise). Colored top to bottom.
static void r_crescent(Rect dst, V4 top, V4 bottom, F32 cut, F32 offset, F32 angle, F32 border)
{
    R_Rect *r = r_push();
    r->dst[0] = dst.x0; r->dst[1] = dst.y0; r->dst[2] = dst.x1; r->dst[3] = dst.y1;
    r_set_colors(r, top, bottom);
    r->params[1] = border; r->params[2] = 0.5f;
    r->shape[0] = R_SHAPE_CRESCENT; r->shape[1] = cut; r->shape[2] = offset; r->shape[3] = angle;
}

// A leaf (two arcs meeting in points) centered on (cx, cy), hw half wide and hh half tall when
// upright, turned by angle (clockwise). Colored top to bottom.
static void r_leaf(F32 cx, F32 cy, F32 hw, F32 hh, F32 angle, V4 top, V4 bottom, F32 border)
{
    R_Rect *r = r_push();
    F32 e = Max(hw, hh) + 1;
    r->dst[0] = cx - e; r->dst[1] = cy - e; r->dst[2] = cx + e; r->dst[3] = cy + e;
    r_set_colors(r, top, bottom);
    r->params[1] = border; r->params[2] = 0.5f;
    r->shape[0] = R_SHAPE_LEAF; r->shape[1] = Min(hw, hh * 0.98f); r->shape[2] = hh; r->shape[3] = angle;
}

// A stroke from (ax, ay), ra thick (radius), to (bx, by), rb thick: a spike, a blade, a flame's
// tongue. Colored top to bottom.
static void r_taper(F32 ax, F32 ay, F32 bx, F32 by, F32 ra, F32 rb, V4 top, V4 bottom)
{
    R_Rect *r = r_push();
    F32 x0 = Min(ax - ra, bx - rb) - 1, y0 = Min(ay - ra, by - rb) - 1;
    F32 x1 = Max(ax + ra, bx + rb) + 1, y1 = Max(ay + ra, by + rb) + 1;
    F32 mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    r->dst[0] = x0; r->dst[1] = y0; r->dst[2] = x1; r->dst[3] = y1;
    r->src[0] = ax - mx; r->src[1] = ay - my; r->src[2] = bx - mx; r->src[3] = by - my;
    r_set_colors(r, top, bottom);
    r->params[2] = 0.5f;
    r->shape[0] = R_SHAPE_TAPER; r->shape[1] = ra; r->shape[2] = rb;
}

// A soft disc of light centered on (cx, cy): solid out to r - soft, gone at r + soft.
static void r_glow(F32 cx, F32 cy, F32 r, F32 soft, V4 color)
{
    R_Rect *q = r_push();
    F32 e = r + soft + 1;
    q->dst[0] = cx - e; q->dst[1] = cy - e; q->dst[2] = cx + e; q->dst[3] = cy + e;
    q->src[1] = -0.01f; q->src[3] = 0.01f; // (a dot of a stroke)
    r_set_colors(q, color, color);
    q->params[2] = Max(soft, 0.5f);
    q->shape[0] = R_SHAPE_TAPER; q->shape[1] = r; q->shape[2] = r;
}

// A diamond touching the middle of dst's edges, colored top to bottom; border > 0: only its outline.
static void r_diamond(Rect dst, V4 top, V4 bottom, F32 border)
{
    R_Rect *r = r_push();
    r->dst[0] = dst.x0; r->dst[1] = dst.y0; r->dst[2] = dst.x1; r->dst[3] = dst.y1;
    r_set_colors(r, top, bottom);
    r->params[1] = border; r->params[2] = 0.5f;
    r->shape[0] = R_SHAPE_DIAMOND;
}

// Textured quad from the glyph atlas (src in atlas pixels).
static void r_glyph(Rect dst, Rect src, V4 color)
{
    R_Rect *r = r_push();
    F32 iw = 1.0f / (F32)r_state->atlas_w, ih = 1.0f / (F32)r_state->atlas_h;
    r->dst[0] = dst.x0; r->dst[1] = dst.y0; r->dst[2] = dst.x1; r->dst[3] = dst.y1;
    r->src[0] = src.x0 * iw; r->src[1] = src.y0 * ih; r->src[2] = src.x1 * iw; r->src[3] = src.y1 * ih;
    r_set_colors(r, color, color);
    r->params[3] = 1;
}

// Replaces the full-size picture with w x h BGRA pixels (straight alpha); recreated when the size changes.
static B32 r_picture_set(U32 w, U32 h, U8 *pixels)
{
    R_State *r = r_state;
    if (r->picture && (r->picture_w != w || r->picture_h != h)) {
        ID3D11ShaderResourceView_Release(r->picture_srv); r->picture_srv = 0;
        ID3D11Texture2D_Release(r->picture); r->picture = 0;
    }
    if (!r->picture) {
        D3D11_TEXTURE2D_DESC td = { 0 };
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(r->device, &td, 0, &r->picture))) { r->picture = 0; return 0; }
        ID3D11Device_CreateShaderResourceView(r->device, (ID3D11Resource *)r->picture, 0, &r->picture_srv);
        r->picture_w = w; r->picture_h = h;
    }
    ID3D11DeviceContext_UpdateSubresource(r->ctx, (ID3D11Resource *)r->picture, 0, 0, pixels, w * 4, 0);
    return 1;
}

// The full-size picture, stretched to dst.
static void r_picture(Rect dst, V4 tint, F32 radius)
{
    R_Rect *r = r_push();
    r->dst[0] = dst.x0; r->dst[1] = dst.y0; r->dst[2] = dst.x1; r->dst[3] = dst.y1;
    r->src[0] = 0; r->src[1] = 0; r->src[2] = 1; r->src[3] = 1;
    r_set_colors(r, tint, tint);
    r->params[0] = radius; r->params[2] = 0.5f; r->params[3] = 3;
}

// A picture from the image atlas (src in atlas pixels), tinted, with rounded corners.
static void r_image(Rect dst, Rect src, V4 tint, F32 radius)
{
    R_Rect *r = r_push();
    F32 iw = 1.0f / (F32)r_state->images_w, ih = 1.0f / (F32)r_state->images_h;
    r->dst[0] = dst.x0; r->dst[1] = dst.y0; r->dst[2] = dst.x1; r->dst[3] = dst.y1;
    r->src[0] = src.x0 * iw; r->src[1] = src.y0 * ih; r->src[2] = src.x1 * iw; r->src[3] = src.y1 * ih;
    r_set_colors(r, tint, tint);
    r->params[0] = radius; r->params[2] = 0.5f; r->params[3] = 2;
}

// Testing: when set, the next frame is also saved there as a 32-bit BMP (what was rendered, so it
// works without a visible desktop, e.g. while the screen is locked).
static WCHAR r_shot_path[512];

static void r_save_shot(void)
{
    R_State *r = r_state;
    ID3D11Texture2D *back = 0, *staging = 0;
    if (FAILED(IDXGISwapChain1_GetBuffer(r->swapchain, 0, &IID_ID3D11Texture2D, (void **)&back))) return;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(back, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(r->device, &td, 0, &staging))) {
        ID3D11DeviceContext_CopyResource(r->ctx, (ID3D11Resource *)staging, (ID3D11Resource *)back);
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ID3D11DeviceContext_Map(r->ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map))) {
            HANDLE f = CreateFileW(r_shot_path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
            if (f != INVALID_HANDLE_VALUE) {
                U32 w = td.Width, h = td.Height, size = w * h * 4;
                BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + size), 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
                BITMAPINFOHEADER ih = { sizeof(ih), (LONG)w, -(LONG)h, 1, 32, BI_RGB, size, 0, 0, 0, 0 };
                DWORD wrote;
                WriteFile(f, &fh, sizeof(fh), &wrote, 0);
                WriteFile(f, &ih, sizeof(ih), &wrote, 0);
                for (U32 y = 0; y < h; y += 1) WriteFile(f, (U8 *)map.pData + (SIZE_T)y * map.RowPitch, w * 4, &wrote, 0);
                CloseHandle(f);
            }
            ID3D11DeviceContext_Unmap(r->ctx, (ID3D11Resource *)staging, 0);
        }
        ID3D11Texture2D_Release(staging);
    }
    ID3D11Texture2D_Release(back);
}

static void r_end_frame(V4 clear)
{
    R_State *r = r_state;
    ID3D11DeviceContext *ctx = r->ctx;

    if (r->rect_count) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)r->instances, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, r->rects, sizeof(R_Rect) * r->rect_count);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)r->instances, 0);
        }
    }
    F32 globals[4] = { (F32)r->width, (F32)r->height, 0, 0 };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)r->globals, 0, 0, globals, 0, 0);

    F32 clear_color[4] = { clear.x, clear.y, clear.z, clear.w };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &r->rtv, 0);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, r->rtv, clear_color);

    D3D11_VIEWPORT vp = { 0, 0, (F32)r->width, (F32)r->height, 0, 1 };
    UINT stride = sizeof(R_Rect), offset = 0;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, r->raster);
    ID3D11DeviceContext_OMSetBlendState(ctx, r->blend, 0, 0xffffffff);
    ID3D11DeviceContext_IASetInputLayout(ctx, r->layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &r->instances, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(ctx, r->vs, 0, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &r->globals);
    ID3D11DeviceContext_PSSetShader(ctx, r->ps, 0, 0);
    ID3D11ShaderResourceView *srvs[3] = { r->atlas_srv, r->images_srv, r->picture_srv };
    ID3D11SamplerState *samplers[2] = { r->sampler, r->sampler_linear };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, srvs);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, samplers);

    for (U32 i = 0; i < r->batch_count; i += 1) {
        R_Batch *b = &r->batches[i];
        if (!b->count) continue;
        D3D11_RECT sc = { (LONG)b->clip.x0, (LONG)b->clip.y0, (LONG)b->clip.x1, (LONG)b->clip.y1 };
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
        ID3D11DeviceContext_DrawInstanced(ctx, 4, b->count, 0, b->first);
    }
    if (r_shot_path[0]) { r_save_shot(); r_shot_path[0] = 0; }
    U64 tp = os_now_us();
    IDXGISwapChain1_Present(r->swapchain, 1, 0);
    r->us_present = os_now_us() - tp;
}
