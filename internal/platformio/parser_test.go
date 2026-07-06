package platformio

import (
	"testing"

	"github.com/arnal/corefw/internal/progress"
)

func TestParserDetectsBuildPhases(t *testing.T) {
	parser := NewParser()

	assertEvent(t, parser.ParseLine("stdout", "Compiling .pio/build/heltec/src/main.cpp.o"), progress.PhaseCompile, progress.StatusStarted)
	assertEvent(t, parser.ParseLine("stdout", "Linking .pio/build/heltec/firmware.elf"), progress.PhaseLink, progress.StatusStarted)
	assertEvent(t, parser.ParseLine("stdout", "Checking size .pio/build/heltec/firmware.elf"), progress.PhaseSize, progress.StatusStarted)

	events := parser.ParseLine("stdout", "Flash: [===       ]  34.5% (used 452984 bytes from 1310720 bytes)")
	assertEvent(t, events, progress.PhaseSize, progress.StatusProgress)
	if events[0].Progress == nil || events[0].Progress.Percent == nil {
		t.Fatalf("expected size progress percent")
	}
	if got := *events[0].Progress.Percent; got != 0.345 {
		t.Fatalf("percent = %v, want 0.345", got)
	}
}

func TestParserDetectsUploadProgress(t *testing.T) {
	parser := NewParser()

	assertEvent(t, parser.ParseLine("stdout", "Uploading .pio/build/heltec/firmware.bin"), progress.PhaseUpload, progress.StatusStarted)
	events := parser.ParseLine("stdout", "Writing at 0x00010000... (42 %)")
	assertEvent(t, events, progress.PhaseUpload, progress.StatusProgress)
	if events[0].Progress == nil || events[0].Progress.Percent == nil {
		t.Fatalf("expected upload progress percent")
	}
	if got := *events[0].Progress.Percent; got != 0.42 {
		t.Fatalf("percent = %v, want 0.42", got)
	}
}

func assertEvent(t *testing.T, events []progress.Event, phase progress.Phase, status progress.Status) {
	t.Helper()
	if len(events) == 0 {
		t.Fatalf("expected an event")
	}
	if events[0].Phase != phase || events[0].Status != status {
		t.Fatalf("event = (%s, %s), want (%s, %s)", events[0].Phase, events[0].Status, phase, status)
	}
}
