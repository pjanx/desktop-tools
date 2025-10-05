package main

import (
	"fmt"
	"math/rand"
	"strings"
	"time"

	"janouch.name/desktop-tools/liust-50/charset"
)

const (
	displayWidth  = 20
	displayHeight = 2
	targetCharset = 0x63
)

type DisplayState struct {
	Display [displayHeight][displayWidth]uint8
}

type Display struct {
	Current, Last DisplayState
}

func NewDisplay() *Display {
	t := &Display{}
	for y := 0; y < displayHeight; y++ {
		for x := 0; x < displayWidth; x++ {
			t.Current.Display[y][x] = ' '
			t.Last.Display[y][x] = ' '
		}
	}
	return t
}

func (t *Display) SetLine(row int, content string) {
	if row < 0 || row >= displayHeight {
		return
	}

	runes := []rune(content)
	for x := 0; x < displayWidth; x++ {
		if x < len(runes) {
			b, ok := charset.ResolveRune(runes[x], targetCharset)
			if ok {
				t.Current.Display[row][x] = b
			} else {
				t.Current.Display[row][x] = '?'
			}
		} else {
			t.Current.Display[row][x] = ' '
		}
	}
}

func (t *Display) HasChanges() bool {
	for y := 0; y < displayHeight; y++ {
		for x := 0; x < displayWidth; x++ {
			if t.Current.Display[y][x] != t.Last.Display[y][x] {
				return true
			}
		}
	}
	return false
}

func (t *Display) Update() {
	for y := 0; y < displayHeight; y++ {
		start := -1
		for x := 0; x < displayWidth; x++ {
			if t.Current.Display[y][x] != t.Last.Display[y][x] {
				start = x
				break
			}
		}
		if start >= 0 {
			fmt.Printf("\x1b[%d;%dH%s",
				y+1, start+1, []byte(t.Current.Display[y][start:]))
			copy(t.Last.Display[y][start:], t.Current.Display[y][start:])
		}
	}
}

func statusProducer(lines chan<- string) {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	temperature, fetcher := "", NewWeatherFetcher()
	temperatureChan := make(chan string)
	go fetcher.Run(5*time.Minute, temperatureChan)

	for {
		select {
		case newTemperature := <-temperatureChan:
			temperature = newTemperature
		default:
		}

		now := time.Now()
		status := fmt.Sprintf("%s %3s %s",
			now.Format("Mon _2 Jan"), temperature, now.Format("15:04"))

		// Ensure exactly 20 characters.
		runes := []rune(status)
		if len(runes) > displayWidth {
			status = string(runes[:displayWidth])
		} else if len(runes) < displayWidth {
			status = status + strings.Repeat(" ", displayWidth-len(runes))
		}

		lines <- status
		<-ticker.C
	}
}

func main() {
	rand.Seed(time.Now().UTC().UnixNano())
	terminal := NewDisplay()

	kaomojiChan := make(chan string, 1)
	statusChan := make(chan string, 1)
	go func() {
		kaomojiChan <- strings.Repeat(" ", displayWidth)
		statusChan <- strings.Repeat(" ", displayWidth)
	}()

	go kaomojiProducer(kaomojiChan)
	go statusProducer(statusChan)

	// TODO(p): And we might want to disable cursor visibility as well.
	fmt.Printf("\x1bR%c", targetCharset)
	fmt.Print("\x1b[2J") // Clear display

	for {
		select {
		case line := <-kaomojiChan:
			terminal.SetLine(0, line)
		case line := <-statusChan:
			terminal.SetLine(1, line)
		}
		if terminal.HasChanges() {
			terminal.Update()
		}
	}
}
