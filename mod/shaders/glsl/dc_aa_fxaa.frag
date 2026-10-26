#version 460
// 25.10.2026: СГЛАЖИВАНИЕ FXAA (меню GRAPHICS -> Anti-Aliasing -> FXAA; gpu_vk_raster.cpp aa_run). Своя запись алгоритма
// FXAA 3.11 Quality (Т. Лоттес, NVIDIA), набор шагов 39: перепад яркости в кресте 3x3, направление края, поиск концов края
// вдоль него, сдвиг выборки поперёк края к паре + снятие субпиксельной ступени. Вход - копия кадра после основного прохода
// (цвет в гамме, как в цели); выход - в цель кадра, до SSAO, свечения и дымки.
// Интерфейс, текст и плоские экраны не трогаются: у них маска в альфе (dc_raster.frag aoMaskOut) < 0.25 - если она такая у
// пикселя или у соседа по кресту, пиксель остаётся как есть (итоговая выборка берёт только его и соседа по кресту).
// 25.10.2026 вечер: проход загружает цель кадра (там тот же кадр, что в копии), пиксель без края не пишется (discard) - запись
// только у краёв; без маски дальше (альфа = 1) пишется и пиксель, у которого альфа не 1.
layout(binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    vec4 rt;     // активная область w, h; 1 / ширина и 1 / высота текстуры
    vec4 par;    // x - 1: альфа центра остаётся (маска нужна SSAO и дымке), 0: альфа = 1
} pc;
layout(location = 0) out vec4 o;

const float SUBPIX = 0.75;          // доля снятия субпиксельной ступени (по умолчанию FXAA)
const float EDGE_THR = 0.125;       // порог края от яркости окрестности (высокое качество)
const float EDGE_THR_MIN = 0.0312;  // нижний предел порога (тёмные сцены)
const float STEP[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

ivec2 g_lim;
vec4 px(ivec2 q) { return texelFetch(src, clamp(q, ivec2(0), g_lim), 0); }
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float lumaAt(vec2 uv) {             // билинейно, внутри активной области
    uv = clamp(uv, 0.5 * pc.rt.zw, (pc.rt.xy - 0.5) * pc.rt.zw);
    return luma(textureLod(src, uv, 0.0).rgb);
}
// пиксель как есть: в цели кадра он уже такой - не пишется; без маски дальше пишется альфа 1, если она у пикселя не 1
void unchanged(vec4 c) {
    if (pc.par.x > 0.5 || c.a > 0.999) discard;
    o = vec4(c.rgb, 1.0);
}

void main() {
    g_lim = ivec2(pc.rt.xy) - 1;
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 cM = px(p);
    float aOut = pc.par.x > 0.5 ? cM.a : 1.0;
    vec4 cN = px(p + ivec2(0, -1)), cS = px(p + ivec2(0, 1)), cW = px(p + ivec2(-1, 0)), cE = px(p + ivec2(1, 0));
    if (min(min(cM.a, cN.a), min(min(cS.a, cW.a), cE.a)) < 0.25) { unchanged(cM); return; }
    float lM = luma(cM.rgb), lN = luma(cN.rgb), lS = luma(cS.rgb), lW = luma(cW.rgb), lE = luma(cE.rgb);
    float lMax = max(max(max(lN, lS), max(lW, lE)), lM);
    float lMin = min(min(min(lN, lS), min(lW, lE)), lM);
    float range = lMax - lMin;
    if (range < max(EDGE_THR_MIN, lMax * EDGE_THR)) { unchanged(cM); return; }
    float lNW = luma(px(p + ivec2(-1, -1)).rgb), lNE = luma(px(p + ivec2(1, -1)).rgb);
    float lSW = luma(px(p + ivec2(-1, 1)).rgb), lSE = luma(px(p + ivec2(1, 1)).rgb);
    // направление: край вдоль x (горизонтальный) - перепад по y больше
    float eH = abs(lNW - 2.0 * lW + lSW) + 2.0 * abs(lN - 2.0 * lM + lS) + abs(lNE - 2.0 * lE + lSE);
    float eV = abs(lNW - 2.0 * lN + lNE) + 2.0 * abs(lW - 2.0 * lM + lE) + abs(lSW - 2.0 * lS + lSE);
    bool horz = eH >= eV;
    // субпиксельная доля: насколько центр отличается от среднего 3x3
    float avg = (2.0 * (lN + lS + lW + lE) + (lNW + lNE + lSW + lSE)) / 12.0;
    float sc = clamp(abs(avg - lM) / range, 0.0, 1.0);
    float sub = (-2.0 * sc + 3.0) * sc * sc;
    sub = sub * sub * SUBPIX;
    // пара поперёк края: та сторона, где перепад больше
    float l1 = horz ? lN : lW, l2 = horz ? lS : lE;
    float g1 = l1 - lM, g2 = l2 - lM;
    bool pair1 = abs(g1) >= abs(g2);
    float grad = max(abs(g1), abs(g2));
    float stepLen = horz ? pc.rt.w : pc.rt.z;                 // шаг поперёк края, в текстурных координатах
    if (pair1) stepLen = -stepLen;                            // пара - сосед с меньшей координатой (сверху / слева)
    float lPair = (pair1 ? l1 : l2) + lM;                     // сумма яркостей по обе стороны края
    vec2 posM = (vec2(p) + 0.5) * pc.rt.zw;
    vec2 posB = posM;
    if (horz) posB.y += stepLen * 0.5; else posB.x += stepLen * 0.5;
    vec2 off = horz ? vec2(pc.rt.z, 0.0) : vec2(0.0, pc.rt.w);
    // поиск концов края в обе стороны: конец - где яркость на линии края ушла от средней пары больше чем на четверть перепада
    vec2 posN = posB - off * STEP[0], posP = posB + off * STEP[0];
    float gScaled = grad * 0.25;
    float eN = lumaAt(posN) - lPair * 0.5, eP = lumaAt(posP) - lPair * 0.5;
    bool doneN = abs(eN) >= gScaled, doneP = abs(eP) >= gScaled;
    if (!doneN) posN -= off * STEP[1];
    if (!doneP) posP += off * STEP[1];
    for (int i = 2; i < 12 && !(doneN && doneP); i++) {
        if (!doneN) eN = lumaAt(posN) - lPair * 0.5;
        if (!doneP) eP = lumaAt(posP) - lPair * 0.5;
        doneN = abs(eN) >= gScaled; doneP = abs(eP) >= gScaled;
        if (!doneN) posN -= off * STEP[i];
        if (!doneP) posP += off * STEP[i];
    }
    float dN = horz ? posM.x - posN.x : posM.y - posN.y;
    float dP = horz ? posP.x - posM.x : posP.y - posM.y;
    bool nearN = dN < dP;
    float dist = min(dN, dP);
    float span = dN + dP;
    bool lMltZ = (lM - lPair * 0.5) < 0.0;
    bool goodSpan = ((nearN ? eN : eP) < 0.0) != lMltZ;      // ближний конец - по ту сторону от центра: ступень настоящая
    float pixOff = goodSpan ? (0.5 - dist / span) : 0.0;
    pixOff = max(pixOff, sub);
    vec2 uv = posM;
    if (horz) uv.y += pixOff * stepLen; else uv.x += pixOff * stepLen;
    uv = clamp(uv, 0.5 * pc.rt.zw, (pc.rt.xy - 0.5) * pc.rt.zw);
    o = vec4(textureLod(src, uv, 0.0).rgb, aOut);
}
