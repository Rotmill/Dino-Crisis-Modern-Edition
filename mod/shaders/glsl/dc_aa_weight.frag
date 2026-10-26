#version 460
// 25.10.2026: СГЛАЖИВАНИЕ SMAA и CMAA2, проход 2 из 3 - веса смешения вдоль длинных рёбер (gpu_vk_raster.cpp aa_run).
// Для каждого ребра пикселя (сверху и слева) ищутся концы его линии (до MAXD пикселей в каждую сторону) и поперечные рёбра на
// концах - по ним форма ступени (L, Z, U); вес смешения пары пикселей по обе стороны ребра - доля площади пикселя под
// восстановленной наклонной линией. Площадь считается прямо (у SMAA она в заранее рассчитанной текстуре). Поиска диагоналей нет.
// SMAA (без -D): формы как у SMAA 1x, малые U скругляются сильнее, у углов фигур смешение слабее.
// CMAA2 (-DDC_CMAA=1): только настоящие ступени линии (за поперечным ребром линия идёт дальше на соседнем ряду); углы фигур и
// одиночные выступы - простые формы прохода 3.
// Выход RGBA8: rg - ребро сверху (r - пиксель берёт у верхнего, g - верхний у пикселя), ba - ребро слева (b - пиксель у левого,
// a - левый у пикселя). 25.10.2026 вечер: цель залита нулём (aa_run), пишутся только пиксели с весами (discard).
layout(binding = 1) uniform sampler2D edg;
layout(push_constant) uniform PC {
    vec4 rt;     // активная область w, h; 1 / ширина и 1 / высота текстуры
    vec4 par;    // не используется
} pc;
layout(location = 0) out vec4 o;

const int MAXD = 48;                // дальность поиска конца линии, пикселей в каждую сторону

ivec2 g_lim;
bool g_hz;                          // линия горизонтальная (ребро сверху); иначе - вертикальная (ребро слева)
// рёбра в осях линии: u - вдоль, v - поперёк. .x - поперечное ребро в начале клетки u (у горизонтальной линии - левое ребро
// пикселя), .y - ребро самой линии (у горизонтальной - верхнее). Вне активной области рёбер нет.
vec2 L2(int u, int v) {
    ivec2 q = g_hz ? ivec2(u, v) : ivec2(v, u);
    if (any(lessThan(q, ivec2(0))) || any(greaterThan(q, g_lim))) return vec2(0.0);
    vec2 e = texelFetch(edg, q, 0).rg;
    return g_hz ? e : e.yx;
}

// доля площади пикселя [x, x+1] между ребром (y = 0) и прямой p1-p2 (только над отрезком p1.x..p2.x):
// .x - по сторону пикселя (y < 0: пиксель берёт у соседа через ребро), .y - по сторону соседа (сосед берёт у пикселя)
vec2 areaLine(vec2 p1, vec2 p2, float x) {
    float x1 = x, x2 = x + 1.0;
    if (!((x1 >= p1.x && x1 < p2.x) || (x2 > p1.x && x2 <= p2.x))) return vec2(0.0);
    vec2 dd = p2 - p1;
    float y1 = p1.y + dd.y * (x1 - p1.x) / dd.x, y2 = p1.y + dd.y * (x2 - p1.x) / dd.x;
    if (sign(y1) == sign(y2) || abs(y1) < 1e-4 || abs(y2) < 1e-4) {      // трапеция по одну сторону ребра
        float a = (y1 + y2) * 0.5;
        return a < 0.0 ? vec2(-a, 0.0) : vec2(0.0, a);
    }
    float xc = p1.x - p1.y * dd.x / dd.y;                                // прямая пересекает ребро внутри пикселя
    float a1 = xc > p1.x ? abs(y1) * (xc - x1) * 0.5 : 0.0;
    float a2 = xc < p2.x ? abs(y2) * (x2 - xc) * 0.5 : 0.0;
    return y1 < 0.0 ? vec2(a1, a2) : vec2(a2, a1);
}

vec2 edgeArea(bool hz, ivec2 p) {
    g_hz = hz;
    int u = hz ? p.x : p.y, v = hz ? p.y : p.x, lim = hz ? g_lim.x : g_lim.y;
    // к началу (влево / вверх): пока на границе клетки нет поперечного ребра и линия идёт дальше
    int ul = u;
    vec2 cur = L2(u, v);
    float lU = L2(u, v - 1).x;
    for (int i = 0; i < MAXD && ul > 0 && cur.x < 0.5 && lU < 0.5; i++) {
        vec2 nx = L2(ul - 1, v);
        if (nx.y < 0.5) break;
        ul--; cur = nx; lU = L2(ul, v - 1).x;
    }
    float lD = cur.x;
    // к концу (вправо / вниз)
    int ur = u;
    vec2 nx = L2(u + 1, v);
    float rU = L2(u + 1, v - 1).x;
    for (int i = 0; i < MAXD && ur < lim && nx.x < 0.5 && rU < 0.5 && nx.y > 0.5; i++) {
        ur++; nx = L2(ur + 1, v); rU = L2(ur + 1, v - 1).x;
    }
    float rD = nx.x;
    float dl = float(u - ul), dr = float(ur - u), d = dl + dr + 1.0;
    // формы концов: 1 - поперечное ребро по сторону пикселя (прямая уходит к нему, y = -0.5), 2 - по сторону соседа (y = 0.5)
#ifdef DC_CMAA
    bool sLD = lD > 0.5 && L2(ul - 1, v + 1).y > 0.5, sLU = lU > 0.5 && L2(ul - 1, v - 1).y > 0.5;
    bool sRD = rD > 0.5 && L2(ur + 1, v + 1).y > 0.5, sRU = rU > 0.5 && L2(ur + 1, v - 1).y > 0.5;
    int L = sLD != sLU ? (sLD ? 1 : 2) : 0, R = sRD != sRU ? (sRD ? 1 : 2) : 0;
#else
    int L = int(lD > 0.5) + 2 * int(lU > 0.5), R = int(rD > 0.5) + 2 * int(rU > 0.5);
    if (L == 3) L = R == 1 ? 2 : (R == 2 ? 1 : 0);              // рёбра по обе стороны: продолжение Z от другого конца
    if (R == 3) R = L == 1 ? 2 : (L == 2 ? 1 : 0);
#endif
    if (L == 0 && R == 0) return vec2(0.0);
    float yL = L == 1 ? -0.5 : 0.5, yR = R == 1 ? -0.5 : 0.5;
    if (L != 0 && R != 0 && L != R) return areaLine(vec2(0.0, yL), vec2(d, yR), dl);          // Z: одна прямая через обе ступени
    vec2 a1 = L != 0 ? areaLine(vec2(0.0, yL), vec2(d * 0.5, 0.0), dl) : vec2(0.0);        // L и U: по половине у каждой ступени
    vec2 a2 = R != 0 ? areaLine(vec2(d * 0.5, 0.0), vec2(d, yR), dl) : vec2(0.0);
#ifdef DC_CMAA
    return a1 + a2;
#else
    if (L != 0 && R != 0) {                                     // U: короткие скругляются сильнее
        float t = clamp(d / 32.0, 0.0, 1.0);
        a1 = mix(sqrt(a1 * 2.0) * 0.5, a1, t);
        a2 = mix(sqrt(a2 * 2.0) * 0.5, a2, t);
    }
    // угол фигуры: поперечное ребро идёт дальше ступени - смешение слабее (у середины линии - вполовину от обоих концов)
    vec2 lr = step(vec2(dl, dr), vec2(dr, dl));
    vec2 rnd = 0.75 * lr / (lr.x + lr.y);
    vec2 f = vec2(1.0) - vec2(rnd.x * L2(ul, v + 1).x + rnd.y * L2(ur + 1, v + 1).x,
                              rnd.x * L2(ul, v - 2).x + rnd.y * L2(ur + 1, v - 2).x);
    return (a1 + a2) * clamp(f, 0.0, 1.0);
#endif
}

void main() {
    g_lim = ivec2(pc.rt.xy) - 1;
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 e = texelFetch(edg, p, 0).rg;
    if (max(e.r, e.g) < 0.5) discard;
    vec4 w = vec4(0.0);
    if (e.g > 0.5) w.rg = edgeArea(true, p);
    if (e.r > 0.5) w.ba = edgeArea(false, p);
    if (dot(w, vec4(1.0)) < 1e-5) discard;
    o = w;
}
