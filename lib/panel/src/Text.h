// Text on the panel: UTF-8 in, Inter GFX fonts out.
// The generated fonts pack °, µ, ³, ₂, —, ·, ▲, ▼ after '~' (tools/mkfonts.py);
// toGlyphs() maps UTF-8 onto those codes and drops anything else to '?'.
#pragma once
#include <Adafruit_GFX.h>

namespace panel {

namespace font {
extern const GFXfont *Small;   // 16 px  footer, tags
extern const GFXfont *Body;    // 21 px  list rows, small labels
extern const GFXfont *Label;   // 26 px  tile labels, units
extern const GFXfont *Head;    // 28 px  header, banner
extern const GFXfont *NumS;    // 34 px  small values (full ASCII)
extern const GFXfont *NumM;    // 54 px  digits only
extern const GFXfont *NumL;    // 84 px  digits only
extern const GFXfont *NumXL;   // 128 px digits only
}  // namespace font

// UTF-8 → font codes. Returns length written (out is always terminated).
size_t toGlyphs(const char *utf8, char *out, size_t n);

// Height above the baseline of capital letters / digits.
int ascent(const GFXfont *f);
// Advance width of an already-mapped string.
int widthGlyphs(const GFXfont *f, const char *glyphs);
// Advance width of UTF-8 text.
int textWidth(const GFXfont *f, const char *utf8);

enum Align { Left, Center, Right };
// Draws UTF-8 text with its baseline at y. Returns the drawn width.
int drawText(Adafruit_GFX &d, const GFXfont *f, int x, int y, const char *utf8, uint16_t color, Align a = Left);
// Like drawText, but shortens with "…"-style ".." to fit maxW.
int drawTextFit(Adafruit_GFX &d, const GFXfont *f, int x, int y, const char *utf8, uint16_t color, int maxW,
                Align a = Left);

}  // namespace panel
