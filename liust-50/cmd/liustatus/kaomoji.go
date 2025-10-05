package main

import (
	"math/rand"
	"strings"
	"time"
)

type kaomojiKind int

const (
	kaomojiKindAwake kaomojiKind = iota
	kaomojiKindBlink
	kaomojiKindFace
	kaomojiKindChase
	kaomojiKindHappy
	kaomojiKindSleep
	kaomojiKindSnore
	kaomojiKindPeek
)

type kaomojiState struct {
	kind    kaomojiKind
	face    string
	message string
	delay   int
}

func (ks *kaomojiState) Format() string {
	line := []rune(strings.Repeat(" ", displayWidth))

	face := []rune(ks.face)
	if x := (len(line) - len(face) + 1) / 2; x < 0 {
		copy(line, face)
	} else {
		copy(line[x:], face)
	}

	if ks.message != "" {
		copy(line[14:], []rune(ks.message))
	}
	return string(line)
}

func (ks *kaomojiState) Duration() time.Duration {
	return time.Millisecond * time.Duration(ks.delay)
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

func kaomojiNewAwake() kaomojiState {
	return kaomojiState{
		kind:    kaomojiKindAwake,
		face:    "(o_o)",
		message: "",
		delay:   2_000 + rand.Intn(4_000),
	}
}

func kaomojiNewBlink() kaomojiState {
	return kaomojiState{
		kind:    kaomojiKindBlink,
		face:    "(-_-)",
		message: "",
		delay:   100 + rand.Intn(50),
	}
}

func kaomojiNewFace() kaomojiState {
	faces := []struct {
		face, message string
	}{
		{"(x_x)", "ｽﾞｷｽﾞｷ"},
		{"(T_T)", "ｽﾞｰﾝ"},
		{"=^.^=", "ﾆｬｰ"},
		{"(>_<)", "ｹﾞｯﾌﾟ"},
		{"(O_O)", "ｼﾞｰ"},
	}

	x := faces[rand.Intn(len(faces))]
	return kaomojiState{
		kind:    kaomojiKindFace,
		face:    x.face,
		message: x.message,
		delay:   10_000,
	}
}

func kaomojiNewChase() kaomojiState {
	faces := []string{"(ﾟﾛﾟ)", "(ﾟ∩ﾟ)"}
	return kaomojiState{
		kind:    kaomojiKindChase,
		face:    faces[rand.Intn(len(faces))],
		message: "",
		delay:   125,
	}
}

func kaomojiNewHappy() kaomojiState {
	return kaomojiState{
		kind:    kaomojiKindHappy,
		face:    "(^_^)",
		message: "",
		delay:   500,
	}
}

func kaomojiNewSleep() kaomojiState {
	return kaomojiState{
		kind:    kaomojiKindSleep,
		face:    "(-_-)",
		message: "",
		delay:   10_000,
	}
}

func kaomojiNewSnore() kaomojiState {
	return kaomojiState{
		kind:    kaomojiKindSnore,
		face:    "(-_-)",
		message: "ｸﾞｰｸﾞｰ",
		delay:   10_000,
	}
}

func kaomojiNewPeek() kaomojiState {
	faces := []string{"(o_-)", "(-_o)"}
	return kaomojiState{
		kind:    kaomojiKindPeek,
		face:    faces[rand.Intn(len(faces))],
		message: "",
		delay:   3_000,
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

func kaomojiAnimateChase(state kaomojiState) (lines []string) {
	// The main character is fixed and of fixed width.
	var (
		normal    = []rune("(o_o)")
		alert     = []rune("(O_O)")
		centre    = (displayWidth - 4) / 2
		chaserLen = len([]rune(state.face))
	)

	// For simplicity, let the animation run off-screen.
	for chaserX := chaserLen + displayWidth; chaserX >= 0; chaserX-- {
		line := []rune(strings.Repeat(" ", chaserLen+displayWidth))

		chased, chasedX := normal, chaserLen+centre
		if chasedX > chaserX-7 {
			chased, chasedX = alert, chaserX-7
		}
		if chasedX >= 0 {
			copy(line[chasedX:], chased)
		}

		copy(line[chaserX:], []rune(state.face))
		lines = append(lines, string(line[chaserLen:]))
	}

	// Return our main character back.
	for chasedX := displayWidth; chasedX >= centre; chasedX-- {
		line := []rune(strings.Repeat(" ", displayWidth))
		copy(line[chasedX:], normal)
		lines = append(lines, string(line))
	}
	return
}

func kaomojiProducer(lines chan<- string) {
	state := kaomojiNewAwake()
	execute := func() {
		lines <- state.Format()
		time.Sleep(state.Duration())
	}

	for {
		switch state.kind {
		case kaomojiKindAwake:
			execute()
			switch f := rand.Float32(); {
			case f < 0.025:
				state = kaomojiNewFace()
			case f < 0.050:
				state = kaomojiNewChase()
			case f < 0.075:
				state = kaomojiNewHappy()
			case f < 0.100:
				state = kaomojiNewSleep()
			default:
				state = kaomojiNewBlink()
			}

		case kaomojiKindBlink, kaomojiKindFace:
			execute()
			state = kaomojiNewAwake()

		case kaomojiKindHappy:
			face := state.face
			execute()
			state.face = "  " + face
			execute()
			state.face = face
			execute()
			state.face = face + "  "
			execute()
			state.face = face
			execute()
			state = kaomojiNewAwake()

		case kaomojiKindChase:
			for _, line := range kaomojiAnimateChase(state) {
				lines <- line
				time.Sleep(state.Duration())
			}
			state = kaomojiNewAwake()

		case kaomojiKindSleep:
			execute()
			switch f := rand.Float32(); {
			case f < 0.10:
				state = kaomojiNewAwake()
			case f < 0.20:
				state = kaomojiNewPeek()
			case f < 0.60:
				state = kaomojiNewSnore()
			default:
				state = kaomojiNewSleep()
			}

		case kaomojiKindSnore:
			execute()
			state = kaomojiNewSleep()

		case kaomojiKindPeek:
			execute()
			state = kaomojiNewSleep()
		}
	}
}
