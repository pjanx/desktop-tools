package charset

import (
	"bytes"
	_ "embed"
	"image"
	_ "image/png"
	"log"
)

// Charsets are loosely based on CP 437 and JIS X 0201.

var runesJapan2 = [256]rune{
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
	0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
	0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
	0x58, 0x59, 0x5A, 0x5B, '¥', 0x5D, 0x5E, 0x5F,
	0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
	0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
	0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
	0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, '⌂',
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	'▒', '｡', '｢', '｣', '､', '･', 'ｦ', 'ｧ',
	'ｨ', 'ｩ', 'ｪ', 'ｫ', 'ｬ', 'ｭ', 'ｮ', 'ｯ',
	'ｰ', 'ｱ', 'ｲ', 'ｳ', 'ｴ', 'ｵ', 'ｶ', 'ｷ',
	'ｸ', 'ｹ', 'ｺ', 'ｻ', 'ｼ', 'ｽ', 'ｾ', 'ｿ',
	'ﾀ', 'ﾁ', 'ﾂ', 'ﾃ', 'ﾄ', 'ﾅ', 'ﾆ', 'ﾇ',
	'ﾈ', 'ﾉ', 'ﾊ', 'ﾋ', 'ﾌ', 'ﾍ', 'ﾎ', 'ﾏ',
	'ﾐ', 'ﾑ', 'ﾒ', 'ﾓ', 'ﾔ', 'ﾕ', 'ﾖ', 'ﾗ',
	'ﾘ', 'ﾙ', 'ﾚ', 'ﾛ', 'ﾜ', 'ﾝ', 'ﾞ', 'ﾟ',
	'α', 'ß', 'Γ', 'π', 'Σ', 'σ', 'µ', 'τ',
	'Φ', 'Θ', 'Ω', 'δ', '∞', 'φ', 'ε', '∩',
	'→', '←', '↓', '↑', '½', '¼', '★', '◊',
	'㎏', '℔', '�', '×', '▾', '▴', '日', ' ',
}

var runesInternational = [256]rune{
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
	0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
	0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
	0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
	0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
	0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
	0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
	0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, '⌂',
	'Ç', 'ü', 'é', 'â', 'ä', 'à', 'å', 'ç',
	'ê', 'ë', 'è', 'ï', 'î', 'ì', 'Ä', 'Å',
	'É', 'æ', 'Æ', 'ô', 'ö', 'ò', 'û', 'ù',
	'ÿ', 'Ö', 'Ü', '¢', '£', '¥', '₧', 'ƒ',
	'á', 'í', 'ó', 'ú', 'ñ', 'Ñ', 'ª', 'º',
	'¿', '⌐', '¬', '½', '¼', '¡', '«', '»',
	'░', '▒', '▓' - 1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, '█', '▄', '▌', '▐', '▀',
	'α', 'ß', 'Γ', 'π', 'Σ', 'σ', 'µ', 'τ',
	'Φ', 'Θ', 'Ω', 'δ', '∞', 'φ', 'ε', '∩',
	'≡', '±', '≥', '≤', '⌠', '⌡', '÷', '≈',
	'°', '∙', '·', '√', 'ⁿ', '²', '■', ' ',
}

var runesInternationalVariants = []string{
	"#$@[\\]^`{|}~", // USA
	"#$à·ç§^`éùè╍",  // France
	"#$§ÄÖÜ^`äöüß",  // Germany
	"£$@[\\]^`{|}~", // UK
	"#$@ÆØÅ^`æøå~",  // Denmark 1
	"#¤ÉÄÖÅÜéäöåü",  // Sweden
	"#$@·\\é^ùàòèì", // Italy
	"₧$@¡Ñ¿^`╍ñ}~",  // Spain
	"#$@[¥]^`{|}~",  // Japan
	"#¤ÉÆØÅÜéæøåü",  // Norway
	"#$ÉÆØÅÜéæøåü",  // Denmark 2
	"#$á¡Ñ¿é`íñóú",  // Spain 2
	"#$á¡Ñ¿éüíñóú",  // Latin America
}

var internationalVariantsChars = []byte{
	0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x60, 0x7B, 0x7C, 0x7D, 0x7E}

// ResolveCharToRune tries to decode a character into a Unicode rune.
// It may return rune(-1) if the character is deemed to have no representation.
func ResolveCharToRune(char, charset uint8) rune {
	if charset == 0x63 {
		return runesJapan2[char]
	}
	if int(charset) >= len(runesInternationalVariants) {
		return -1
	}

	for i, b := range internationalVariantsChars {
		if char == b {
			return []rune(runesInternationalVariants[charset])[i]
		}
	}
	return runesInternational[char]
}

// ResolveRune tries to find a corresponding character for a Unicode rune.
func ResolveRune(r rune, charset uint8) (uint8, bool) {
	if charset == 0x63 {
		for i, ch := range runesJapan2 {
			if ch == r {
				return uint8(i), true
			}
		}
		return 0, false
	}
	if int(charset) >= len(runesInternationalVariants) {
		return 0, false
	}

	variantRunes := []rune(runesInternationalVariants[charset])
	for i, ch := range variantRunes {
		if ch == r {
			return internationalVariantsChars[i], true
		}
	}
	for i, ch := range runesInternational {
		if ch == r {
			return uint8(i), true
		}
	}
	return 0, false
}

//go:embed japan.png
var pngJapan2 []byte
var imageJapan2 image.Image

//go:embed germany.png
var pngGermany []byte
var imageGermany image.Image

//go:embed international.png
var pngInternational []byte
var imageInternational image.Image

func init() {
	var err error
	imageJapan2, _, err = image.Decode(bytes.NewReader(pngJapan2))
	if err != nil {
		log.Fatalln(err)
	}

	imageGermany, _, err = image.Decode(bytes.NewReader(pngGermany))
	if err != nil {
		log.Fatalln(err)
	}

	imageInternational, _, err = image.Decode(bytes.NewReader(pngInternational))
	if err != nil {
		log.Fatalln(err)
	}
}

// ResolveCharToImage tries to decode a character into a 5x7 bitmap image
// (white on black).
func ResolveCharToImage(char, charset uint8) image.Image {
	const (
		gridWidth  = 6
		gridHeight = 8
	)

	var src image.Image
	var col, row int
	if charset == 0x63 {
		src, col, row = imageJapan2, int(char)/16, int(char)%16
	} else if int(charset) < len(runesInternationalVariants) {
		src, col, row = imageGermany, int(char)/16, int(char)%16
		for i, b := range internationalVariantsChars {
			if char == b {
				src, col, row = imageInternational, i, int(charset)
			}
		}
	} else {
		return nil
	}

	x0 := col * gridWidth
	y0 := row * gridHeight
	return src.(interface {
		SubImage(r image.Rectangle) image.Image
	}).SubImage(image.Rect(
		x0,
		y0,
		x0+gridWidth-1,
		y0+gridHeight-1,
	))
}
