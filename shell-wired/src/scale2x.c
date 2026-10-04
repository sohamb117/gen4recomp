/*
 * Scale2x rules, for pixel P with neighbours B (up), D (left), F (right),
 * H (down); edges repeat the border pixel:
 *   E0 = D == B && B != F && D != H ? D : P   (top-left)
 *   E1 = B == F && B != D && F != H ? F : P   (top-right)
 *   E2 = D == H && D != B && H != F ? D : P   (bottom-left)
 *   E3 = H == F && D != H && B != F ? F : P   (bottom-right)
 */
#include "scale2x.h"

void np_scale2x(const uint32_t *src, int w, int h, size_t stride, uint32_t *dst)
{
    size_t dw = (size_t)w * 2;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = src + (size_t)y * stride;
        const uint32_t *up = y > 0 ? row - stride : row;
        const uint32_t *down = y + 1 < h ? row + stride : row;
        uint32_t *o0 = dst + (size_t)y * 2 * dw, *o1 = o0 + dw;
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x], b = up[x], hh = down[x];
            uint32_t d = x > 0 ? row[x - 1] : p, f = x + 1 < w ? row[x + 1] : p;
            if (b != hh && d != f) {
                o0[2 * x] = d == b ? d : p;
                o0[2 * x + 1] = b == f ? f : p;
                o1[2 * x] = d == hh ? d : p;
                o1[2 * x + 1] = hh == f ? f : p;
            } else {
                o0[2 * x] = o0[2 * x + 1] = o1[2 * x] = o1[2 * x + 1] = p;
            }
        }
    }
}
