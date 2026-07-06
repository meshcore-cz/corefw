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

	assertContainsEvent(t, parser.ParseLine("stdout", "Uploading .pio/build/heltec/firmware.bin"), progress.PhaseUpload, progress.StatusStarted)
	events := parser.ParseLine("stdout", "Writing at 0x00010000... (42 %)")
	assertEvent(t, events, progress.PhaseUpload, progress.StatusProgress)
	if events[0].Progress == nil || events[0].Progress.Percent == nil {
		t.Fatalf("expected upload progress percent")
	}
	if got := *events[0].Progress.Percent; got != 0.42 {
		t.Fatalf("percent = %v, want 0.42", got)
	}
}

func TestParserCompletesBuildPhasesWhenUploadStarts(t *testing.T) {
	parser := NewParser()

	parser.ParseLine("stdout", "Building in release mode")
	parser.ParseLine("stdout", "Linking .pio/build/env/firmware.elf")
	parser.ParseLine("stdout", "Checking size .pio/build/env/firmware.elf")
	parser.ParseLine("stdout", "Flash: [==        ]  22.8% (used 760821 bytes from 3342336 bytes)")

	events := parser.ParseLine("stdout", "Configuring upload protocol...")
	assertContainsEvent(t, events, progress.PhaseCompile, progress.StatusCompleted)
	assertContainsEvent(t, events, progress.PhaseLink, progress.StatusCompleted)
	assertContainsEvent(t, events, progress.PhaseSize, progress.StatusCompleted)
	assertContainsEvent(t, events, progress.PhaseUpload, progress.StatusStarted)
}

func TestParserSkipsMissingLinkWhenUploadStarts(t *testing.T) {
	parser := NewParser()

	parser.ParseLine("stdout", "Building in release mode")
	parser.ParseLine("stdout", "Checking size .pio/build/env/firmware.elf")

	events := parser.ParseLine("stdout", "Uploading .pio/build/env/firmware.bin")
	assertContainsEvent(t, events, progress.PhaseCompile, progress.StatusCompleted)
	assertContainsEvent(t, events, progress.PhaseLink, progress.StatusSkipped)
	assertContainsEvent(t, events, progress.PhaseSize, progress.StatusCompleted)
	assertContainsEvent(t, events, progress.PhaseUpload, progress.StatusStarted)
}

func TestParserCompleteClosesSkippedAndCompletedPhases(t *testing.T) {
	parser := NewParser()

	parser.ParseLine("stdout", "Building in release mode")
	parser.ParseLine("stdout", "Checking size .pio/build/env/firmware.elf")
	parser.ParseLine("stdout", "Flash: [==        ]  22.8% (used 760821 bytes from 3342336 bytes)")
	parser.ParseLine("stdout", "Auto-detected: /dev/cu.usbserial-0001")

	startEvents := parser.ParseLine("stdout", "Uploading .pio/build/env/firmware.bin")
	assertContainsEvent(t, startEvents, progress.PhaseCompile, progress.StatusCompleted)
	assertContainsEvent(t, startEvents, progress.PhaseLink, progress.StatusSkipped)
	assertContainsEvent(t, startEvents, progress.PhaseSize, progress.StatusCompleted)

	events := parser.Complete(true, "build/env/platformio.log")
	assertContainsEvent(t, events, progress.PhaseUpload, progress.StatusCompleted)

	last := events[len(events)-1]
	if last.Message != "Flashed firmware" {
		t.Fatalf("last message = %q, want flash result", last.Message)
	}
	if last.Detail != "/dev/cu.usbserial-0001 · build/env/platformio.log" {
		t.Fatalf("last detail = %q", last.Detail)
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

func assertContainsEvent(t *testing.T, events []progress.Event, phase progress.Phase, status progress.Status) {
	t.Helper()
	for _, event := range events {
		if event.Phase == phase && event.Status == status {
			return
		}
	}
	t.Fatalf("events did not contain (%s, %s): %+v", phase, status, events)
}
