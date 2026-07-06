package progress

import (
	"fmt"
	"io"
	"sync"
)

// PlainReporter writes compact, non-interactive progress for CI and pipes.
type PlainReporter struct {
	mu          sync.Mutex
	w           io.Writer
	lastPercent map[Phase]int
}

// NewPlainReporter returns a Reporter that writes human-readable lines to w.
func NewPlainReporter(w io.Writer) *PlainReporter {
	return &PlainReporter{
		w:           w,
		lastPercent: make(map[Phase]int),
	}
}

// Report writes selected events as stable, line-oriented text.
func (r *PlainReporter) Report(event Event) {
	if r == nil || r.w == nil {
		return
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	line, ok := r.format(event)
	if !ok {
		return
	}
	fmt.Fprintln(r.w, line)
}

func (r *PlainReporter) format(event Event) (string, bool) {
	switch event.Status {
	case StatusStarted:
		return fmt.Sprintf("start %s: %s", event.Phase, fallbackMessage(event)), true
	case StatusCompleted:
		return fmt.Sprintf("done  %s: %s", event.Phase, fallbackMessage(event)), true
	case StatusSkipped:
		return fmt.Sprintf("skip  %s: %s", event.Phase, fallbackMessage(event)), true
	case StatusWarning:
		return fmt.Sprintf("warn  %s: %s", event.Phase, fallbackMessage(event)), true
	case StatusFailed:
		return fmt.Sprintf("fail  %s: %s", event.Phase, fallbackMessage(event)), true
	case StatusProgress:
		if event.Level == LevelWarning || event.Level == LevelError {
			return fmt.Sprintf("%s %s: %s", event.Level, event.Phase, fallbackMessage(event)), true
		}
		if event.Progress == nil || event.Progress.Percent == nil {
			return "", false
		}
		bucket := int(*event.Progress.Percent * 10)
		if bucket < 0 {
			bucket = 0
		}
		if bucket > 10 {
			bucket = 10
		}
		if r.lastPercent[event.Phase] == bucket {
			return "", false
		}
		r.lastPercent[event.Phase] = bucket
		return fmt.Sprintf("progress %s: %3.0f%% %s", event.Phase, *event.Progress.Percent*100, event.Message), true
	default:
		return "", false
	}
}

func fallbackMessage(event Event) string {
	if event.Message != "" {
		return event.Message
	}
	if event.Detail != "" {
		return event.Detail
	}
	return string(event.Status)
}
