// shaders.hlsl - the renderer's shaders (render_d3d11.c): one instanced quad per R_Rect, its shape drawn as a
// signed distance. Release builds compile them with fxc (build.bat); debug builds carry this file as a
// resource (res\wam.rc) and compile it when wam starts.

cbuffer Globals : register(b0) { float2 viewport; float2 pad; };
struct VSIn { float4 dst : DST; float4 src : SRC; float4 col : COL; float4 params : PARAMS; float4 col2 : COLB; float4 shape : SHAPE;
              uint vid : SV_VertexID; };
struct PSIn { float4 pos : SV_Position; float2 uv : UV; float2 local : LOCAL; float2 half_size : HSIZE;
              float4 col : COL; float4 params : PARAMS; float4 col2 : COLB; float4 shape : SHAPE; float4 src : SRCC; };
PSIn vs_main(VSIn i) {
  float2 corner = float2((i.vid & 1) ? 1.0 : 0.0, (i.vid & 2) ? 1.0 : 0.0);
  float2 p = lerp(i.dst.xy, i.dst.zw, corner);
  PSIn o;
  o.pos = float4(p / viewport * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = lerp(i.src.xy, i.src.zw, corner);
  o.half_size = (i.dst.zw - i.dst.xy) * 0.5;
  o.local = (corner * 2 - 1) * o.half_size;
  o.col = i.col; o.params = i.params; o.col2 = i.col2; o.shape = i.shape; o.src = i.src;
  return o;
}
Texture2D atlas : register(t0); SamplerState samp : register(s0);
Texture2D images : register(t1); SamplerState samp_lin : register(s1);
Texture2D picture : register(t2);
float rrect(float2 p, float2 b, float r) { float2 q = abs(p) - b + r; return length(max(q, 0)) + min(max(q.x, q.y), 0) - r; }
float diamond(float2 p, float2 b) { float2 q = abs(p) / b; return (q.x + q.y - 1) * min(b.x, b.y) * 0.7071; }
// A cog: a disc of the root radius plus teeth, each tooth a sector narrowing from root to tip.
float cog(float2 p, float ro, float teeth, float ratio, float rot) {
  float ri = ro * ratio, r = length(p), seg = 6.2831853 / teeth;
  float f = abs(frac((atan2(p.y, p.x) + rot) / seg) - 0.5) * seg;
  float hw = lerp(seg * 0.30, seg * 0.17, saturate((r - ri) / max(ro - ri, 0.001)));
  return min(r - ri, max(r - ro, (f - hw) * r));
}
float2 rot(float2 p, float a) { float c = cos(a), s = sin(a); return float2(c * p.x + s * p.y, -s * p.x + c * p.y); }
// A star of n points (after Inigo Quilez): m from 2 (spiky) to n (a regular polygon). A point is up.
float star(float2 p, float r, float n, float m) {
  float an = 3.141593 / n, en = 3.141593 / m;
  float2 acs = float2(cos(an), sin(an)), ecs = float2(cos(en), sin(en));
  float a = atan2(p.x, -p.y), w = 2 * an, bn = a - w * floor(a / w) - an;
  p = length(p) * float2(cos(bn), abs(sin(bn)));
  p -= r * acs;
  p += ecs * clamp(-dot(p, ecs), 0.0, r * acs.y / ecs.y);
  return length(p) * sign(p.x);
}
// Two arcs meeting in points at the top and bottom (a vesica): w half wide, h half tall (w < h).
float leaf(float2 p, float w, float h) {
  p = abs(p);
  float d = (h * h / w - w) * 0.5, r = w + d;
  return ((p.y - h) * d > p.x * h) ? length(p - float2(0, h)) : length(p + float2(d, 0)) - r;
}
// A stroke from a (radius ra) to b (radius rb): Inigo Quilez's uneven capsule.
float taper(float2 p, float2 pa, float2 pb, float ra, float rb) {
  p -= pa; pb -= pa;
  float h = max(dot(pb, pb), 0.0001);
  float2 q = float2(dot(p, float2(pb.y, -pb.x)), dot(p, pb)) / h;
  q.x = abs(q.x);
  float b = ra - rb;
  float2 c = float2(sqrt(max(h - b * b, 0.0001)), b);
  float k = c.x * q.y - c.y * q.x, m = dot(c, q), n = dot(q, q);
  if (k < 0.0) return sqrt(h * n) - ra;
  if (k > c.x) return sqrt(h * (n + 1.0 - 2.0 * q.y)) - rb;
  return m - ra;
}
float4 ps_main(PSIn i) : SV_Target {
  float radius = i.params.x, border = i.params.y, soft = i.params.z, kind = i.shape.x;
  float g = (kind < 0.5 && i.shape.y > 0.5) ? i.local.x / max(i.half_size.x, 0.001) : i.local.y / max(i.half_size.y, 0.001);
  float4 c = lerp(i.col, i.col2, saturate(g * 0.5 + 0.5));
  if (i.params.w > 2.5) c *= picture.Sample(samp_lin, i.uv);
  else if (i.params.w > 1.5) c *= images.Sample(samp_lin, i.uv);
  else if (i.params.w > 0.5) c.a *= atlas.Sample(samp, i.uv).r;
  if (soft > 0) {
    float d;
    float rr = min(i.half_size.x, i.half_size.y);
    if (kind > 5.5) d = taper(i.local, i.src.xy, i.src.zw, i.shape.y, i.shape.z);
    else if (kind > 4.5) d = leaf(rot(i.local, i.shape.w), i.shape.y, i.shape.z);
    else if (kind > 3.5) d = max(length(i.local) - rr, i.shape.y * rr - length(i.local - float2(cos(i.shape.w), sin(i.shape.w)) * i.shape.z * rr));
    else if (kind > 2.5) d = star(rot(i.local, i.shape.w), rr, i.shape.y, i.shape.z);
    else if (kind > 1.5) d = diamond(i.local, i.half_size);
    else if (kind > 0.5) d = max(cog(i.local, min(i.half_size.x, i.half_size.y), i.shape.y, i.shape.z, i.shape.w), radius - length(i.local));
    else d = rrect(i.local, i.half_size, radius);
    c.a *= 1 - smoothstep(-soft, soft, d);
    if (border > 0) {
      float inner = kind > 0.5 ? d + border : rrect(i.local, i.half_size - border, max(radius - border, 0));
      c.a *= smoothstep(-soft, soft, inner);
    }
  }
  return c;
}
