package main

import (
	"bufio"
	"image"
	"image/color"
	"log"
	"os"
	"strconv"
	"strings"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	"janouch.name/desktop-tools/liust-50/charset"
)

// --- Display emulation -------------------------------------------------------

const (
	displayWidth  = 20
	displayHeight = 2
	charWidth     = 5 + 1
	charHeight    = 7 + 1
)

// TODO(p): See how this works exactly, and implement it.
const (
	cursorModeOff = iota
	cursorModeBlink
	cursorModeLightUp
)

type Display struct {
	chars      [displayHeight][displayWidth]uint8
	charset    uint8
	cursorX    int
	cursorY    int
	cursorMode int
}

func NewDisplay() *Display {
	return &Display{charset: 2}
}

func (d *Display) Clear() {
	for y := 0; y < displayHeight; y++ {
		for x := 0; x < displayWidth; x++ {
			d.chars[y][x] = 0x20 // space
		}
	}
}

func (d *Display) ClearToEnd() {
	for x := d.cursorX; x < displayWidth; x++ {
		d.chars[d.cursorY][x] = 0x20 // space
	}
}

func (d *Display) drawCharacter(
	img *image.RGBA, character image.Image, cx, cy int) {
	if character == nil {
		return
	}

	bounds := character.Bounds()
	width, height := bounds.Dx(), bounds.Dy()
	for dy := 0; dy < height; dy++ {
		for dx := 0; dx < width; dx++ {
			var c color.RGBA
			if r, _, _, _ := character.At(
				bounds.Min.X+dx, bounds.Min.Y+dy).RGBA(); r >= 0x8000 {
				c = color.RGBA{0x00, 0xFF, 0xB0, 0xFF}
			} else {
				c = color.RGBA{0x18, 0x18, 0x18, 0xFF}
			}
			img.SetRGBA(1+cx*charWidth+dx, 1+cy*charHeight+dy, c)
		}
	}
}

func (d *Display) Render() image.Image {
	width := 1 + displayWidth*charWidth
	height := 1 + displayHeight*charHeight

	// XXX: Not sure if we rather don't want to provide double buffering,
	// meaning we would cycle between two internal buffers.
	img := image.NewRGBA(image.Rect(0, 0, width, height))

	black := [4]uint8{0x00, 0x00, 0x00, 0xFF}
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			copy(img.Pix[img.PixOffset(x, y):], black[:])
		}
	}

	for cy := 0; cy < displayHeight; cy++ {
		for cx := 0; cx < displayWidth; cx++ {
			charImg := charset.ResolveCharToImage(d.chars[cy][cx], d.charset)
			d.drawCharacter(img, charImg, cx, cy)
		}
	}
	return img
}

func (d *Display) PutChar(ch uint8) {
	if d.cursorX >= displayWidth || d.cursorY >= displayHeight {
		return
	}

	d.chars[d.cursorY][d.cursorX] = ch
	d.cursorX++
	if d.cursorX >= displayWidth {
		d.cursorX = displayWidth - 1
	}
}

func (d *Display) LineFeed() {
	d.cursorY++
	if d.cursorY >= displayHeight {
		d.cursorY = displayHeight - 1

		y := 0
		for ; y < displayHeight-1; y++ {
			d.chars[y] = d.chars[y+1]
		}
		for x := 0; x < displayWidth; x++ {
			d.chars[y][x] = 0x20
		}
	}
}

func (d *Display) CarriageReturn() {
	d.cursorX = 0
}

func (d *Display) Backspace() {
	if d.cursorX > 0 {
		d.cursorX--
	}
}

func (d *Display) SetCursor(x, y int) {
	if x >= 0 && x < displayWidth {
		d.cursorX = x
	}
	if y >= 0 && y < displayHeight {
		d.cursorY = y
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

func parseANSI(input string) (command string, params []int) {
	if !strings.HasPrefix(input, "\x1b[") {
		return "", nil
	}

	input = input[2:]
	if len(input) == 0 {
		return "", nil
	}

	cmdIdx := len(input) - 1
	paramStr, command := input[:cmdIdx], input[cmdIdx:]
	if paramStr != "" {
		for _, p := range strings.Split(paramStr, ";") {
			if p = strings.TrimSpace(p); p == "" {
				params = append(params, 0)
			} else if value, err := strconv.Atoi(p); err == nil {
				params = append(params, value)
			}
		}
	}
	return command, params
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

type protocolParser struct {
	seq     strings.Builder
	inEsc   bool
	inCSI   bool
	display *Display
}

func newProtocolParser(d *Display) *protocolParser {
	return &protocolParser{display: d}
}

func (pp *protocolParser) reset() {
	pp.inEsc = false
	pp.inCSI = false
	pp.seq.Reset()
}

func (pp *protocolParser) handleCSICommand() bool {
	cmd, params := parseANSI(pp.seq.String())

	switch cmd {
	case "J": // Clear display
		// XXX: The no params case is unverified.
		if len(params) == 0 || params[0] == 2 {
			pp.display.Clear()
		}
	case "K": // Delete to end of line
		// XXX: The no params case is unverified (but it should work).
		if len(params) == 0 || params[0] == 0 {
			pp.display.ClearToEnd()
		}
	case "H": // Cursor position
		y, x := 0, 0
		if len(params) >= 1 {
			y = params[0] - 1 // 1-indexed to 0-indexed
		}
		if len(params) >= 2 {
			x = params[1] - 1
		}
		pp.display.SetCursor(x, y)
	}
	return true
}

func (pp *protocolParser) handleEscapeSequence(b byte) bool {
	pp.seq.WriteByte(b)

	if pp.seq.Len() == 2 && b == '[' {
		pp.inCSI = true
		return false
	}

	if pp.seq.Len() == 3 && pp.seq.String()[1] == 'R' {
		pp.display.charset = b
		pp.reset()
		return true
	}

	if pp.inCSI && (b >= 'A' && b <= 'Z' || b >= 'a' && b <= 'z') {
		refresh := pp.handleCSICommand()
		pp.reset()
		return refresh
	}

	if pp.seq.Len() == 6 && pp.seq.String()[1:5] == "\\?LC" {
		pp.display.cursorMode = int(pp.seq.String()[5])
		return true
	}

	return false
}

func (pp *protocolParser) handleCharacter(b byte) bool {
	switch b {
	case 0x0A: // LF
		pp.display.LineFeed()
		return true
	case 0x0D: // CR
		pp.display.CarriageReturn()
		return true
	case 0x08: // BS
		pp.display.Backspace()
		return true
	default:
		if b >= 0x20 {
			pp.display.PutChar(b)
			return true
		}
	}
	return false
}

func (pp *protocolParser) handleByte(b byte) (needsRefresh bool) {
	if b == 0x1b { // ESC
		pp.reset()
		pp.inEsc = true
		pp.seq.WriteByte(b)
		return false
	}
	if pp.inEsc {
		return pp.handleEscapeSequence(b)
	}

	return pp.handleCharacter(b)
}

// --- Display widget ----------------------------------------------------------

type DisplayRenderer struct {
	image *canvas.Image
	label *canvas.Text

	objects       []fyne.CanvasObject
	displayWidget *DisplayWidget
}

func (r *DisplayRenderer) Destroy() {}

func (r *DisplayRenderer) Layout(size fyne.Size) {
	minSize := r.MinSize()
	aspectRatio := minSize.Width / minSize.Height

	var areaX, areaY, areaWidth, areaHeight float32
	if size.Width/size.Height > aspectRatio {
		areaHeight = size.Height
		areaWidth = areaHeight * aspectRatio
		areaX = (size.Width - areaWidth) / 2
	} else {
		areaWidth = size.Width
		areaHeight = areaWidth / aspectRatio
		areaY = (size.Height - areaHeight) / 2
	}

	imageHeight := areaHeight * (minSize.Height - 5) / minSize.Height
	r.image.Move(fyne.NewPos(areaX, areaY))
	r.image.Resize(fyne.NewSize(areaWidth, imageHeight))

	// The appropriate TextSize for the desired label height is guesswork.
	// In theory, we could figure out the relation between TextSize
	// and measured height in our MinSize.
	r.label.TextSize = (areaHeight - imageHeight) * 0.75
	labelSize := r.label.MinSize()

	// The VFD display is not mounted exactly in the centre of the device.
	r.label.Move(fyne.NewPos(
		areaX+(areaWidth-labelSize.Width)*0.525,
		areaY+imageHeight))
	r.label.Resize(labelSize)
}

func (r *DisplayRenderer) MinSize() fyne.Size {
	// The VFD display doesn't have rectangular pixels,
	// they are rather elongated in a roughly 3:4 ratio.
	//
	// Add space for the bottom label.
	bounds := r.image.Image.Bounds()
	return fyne.NewSize(float32(bounds.Dx()), float32(bounds.Dy())*1.25).
		AddWidthHeight(0, 5)
}

func (r *DisplayRenderer) Objects() []fyne.CanvasObject { return r.objects }

func (r *DisplayRenderer) Refresh() {
	r.image.Image = r.displayWidget.display.Render()
	r.image.Refresh()
	r.label.Refresh()
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

type DisplayWidget struct {
	widget.BaseWidget
	display *Display
}

func NewDisplayWidget(display *Display) *DisplayWidget {
	dw := &DisplayWidget{display: display}
	dw.ExtendBaseWidget(dw)
	return dw
}

func (dw *DisplayWidget) CreateRenderer() fyne.WidgetRenderer {
	image := canvas.NewImageFromImage(dw.display.Render())
	image.ScaleMode = canvas.ImageScalePixels

	label := canvas.NewText("TOSHIBA", color.Gray{0x99})
	label.TextStyle.Bold = true

	return &DisplayRenderer{
		image:         image,
		label:         label,
		objects:       []fyne.CanvasObject{image, label},
		displayWidget: dw,
	}
}

// --- Main --------------------------------------------------------------------

func main() {
	a := app.New()
	a.Settings().SetTheme(theme.DarkTheme())
	window := a.NewWindow("Toshiba Tec LIUST-50 Simulator")

	display := NewDisplay()
	display.Clear()

	dw := NewDisplayWidget(display)
	window.SetContent(dw)
	window.Resize(fyne.NewSize(600, 150))

	go func() {
		reader := bufio.NewReader(os.Stdin)
		parser := newProtocolParser(display)

		for {
			b, err := reader.ReadByte()
			if err != nil {
				log.Println(err)
				return
			}

			if parser.handleByte(b) {
				fyne.DoAndWait(func() { dw.Refresh() })
			}
		}
	}()

	window.ShowAndRun()
}
