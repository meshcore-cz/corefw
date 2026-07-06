package progress

import "sync"

// Reporter receives structured progress events.
type Reporter interface {
	Report(Event)
}

// ReporterFunc adapts a function to Reporter.
type ReporterFunc func(Event)

// Report calls f with event.
func (f ReporterFunc) Report(event Event) {
	if f != nil {
		f(event)
	}
}

type noopReporter struct{}

// NoopReporter discards all events.
var NoopReporter Reporter = noopReporter{}

func (noopReporter) Report(Event) {}

// Safe returns a reporter that serializes event delivery and recovers from
// reporter panics. Reporting should never decide whether a build succeeds.
func Safe(reporter Reporter) Reporter {
	if reporter == nil {
		reporter = NoopReporter
	}
	return &safeReporter{inner: reporter}
}

type safeReporter struct {
	mu    sync.Mutex
	inner Reporter
}

func (r *safeReporter) Report(event Event) {
	r.mu.Lock()
	defer r.mu.Unlock()
	defer func() {
		_ = recover()
	}()
	r.inner.Report(event)
}
