// Package progress defines UI-neutral build progress events.
package progress

import "time"

// Phase identifies a logical part of the corefw build pipeline.
type Phase string

const (
	PhaseLoadProfile       Phase = "load-profile"
	PhaseFetchComponents   Phase = "fetch-components"
	PhaseResolveComponents Phase = "resolve-components"
	PhaseGenerateProject   Phase = "generate-project"
	PhaseWriteLockfile     Phase = "write-lockfile"
	PhasePrepareToolchain  Phase = "prepare-toolchain"
	PhaseCompile           Phase = "compile"
	PhaseLink              Phase = "link"
	PhaseSize              Phase = "size"
	PhaseUpload            Phase = "upload"
)

// Status identifies the lifecycle state of a progress event.
type Status string

const (
	StatusStarted   Status = "started"
	StatusProgress  Status = "progress"
	StatusCompleted Status = "completed"
	StatusSkipped   Status = "skipped"
	StatusWarning   Status = "warning"
	StatusFailed    Status = "failed"
)

// Level identifies event severity.
type Level string

const (
	LevelInfo    Level = "info"
	LevelWarning Level = "warning"
	LevelError   Level = "error"
	LevelDebug   Level = "debug"
)

// Progress carries optional numeric progress for a phase.
type Progress struct {
	Current int64
	Total   int64
	Percent *float64
}

// Event is a structured, presentation-neutral progress update.
type Event struct {
	Time     time.Time
	Phase    Phase
	Status   Status
	Level    Level
	Message  string
	Detail   string
	Progress *Progress
}
