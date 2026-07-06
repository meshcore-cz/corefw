// Package platformio owns PlatformIO execution and output parsing.
package platformio

import (
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/arnal/corefw/internal/progress"
)

var (
	sizeLineRE      = regexp.MustCompile(`^(RAM|Flash):\s+\[[^\]]*\]\s+([0-9]+(?:\.[0-9]+)?)%`)
	sizeUsedLineRE  = regexp.MustCompile(`^(RAM|Flash):\s+\[[^\]]*\]\s+[0-9]+(?:\.[0-9]+)?%\s+\(used\s+([0-9]+)\s+bytes`)
	uploadPctRE     = regexp.MustCompile(`\(([0-9]+)\s*%\)`)
	nrfHashLineRE   = regexp.MustCompile(`^#+$`)
	dfuTargetLine   = "Upgrading target on "
	dfuActivateLine = "Activating new firmware"
	dfuDoneLine     = "Device programmed."
)

// Parser converts PlatformIO output lines into structured progress events.
type Parser struct {
	mu                   sync.Mutex
	compileStarted       bool
	linkStarted          bool
	sizeStarted          bool
	uploadStarted        bool
	buildPhasesCompleted bool
	uploadPort           string
	flashUsedBytes       int64
	nrfUploadedChunks    int64
}

// NewParser returns a fresh PlatformIO output parser.
func NewParser() *Parser {
	return &Parser{}
}

// ParseLine parses one stdout or stderr line. The stream name is kept generic
// for callers that want to distinguish diagnostics later.
func (p *Parser) ParseLine(stream, line string) []progress.Event {
	p.mu.Lock()
	defer p.mu.Unlock()

	line = strings.TrimSpace(line)
	if line == "" {
		return nil
	}

	lower := strings.ToLower(line)
	var events []progress.Event

	if strings.Contains(lower, "warning") {
		events = append(events, p.event(progress.PhaseCompile, progress.StatusWarning, progress.LevelWarning, "PlatformIO warning", line, nil))
	}
	if strings.Contains(lower, "error") || strings.Contains(line, "[FAILED]") {
		events = append(events, p.event(currentPhase(p), progress.StatusFailed, progress.LevelError, "PlatformIO reported an error", line, nil))
		return events
	}

	switch {
	case strings.HasPrefix(line, "Building in ") ||
		strings.HasPrefix(line, "Compiling ") ||
		strings.HasPrefix(line, "Archiving ") ||
		strings.HasPrefix(line, "Indexing "):
		if !p.compileStarted {
			p.compileStarted = true
			events = append(events, p.event(progress.PhaseCompile, progress.StatusStarted, progress.LevelInfo, "Compiling firmware", line, nil))
		}

	case strings.HasPrefix(line, "Linking "):
		if !p.linkStarted {
			p.linkStarted = true
			events = append(events, p.event(progress.PhaseLink, progress.StatusStarted, progress.LevelInfo, "Linking firmware", line, nil))
		}

	case strings.HasPrefix(line, "Retrieving maximum program size") ||
		strings.HasPrefix(line, "Checking size"):
		if !p.sizeStarted {
			p.sizeStarted = true
			events = append(events, p.event(progress.PhaseSize, progress.StatusStarted, progress.LevelInfo, "Checking firmware size", line, nil))
		}

	case strings.HasPrefix(line, "Configuring upload protocol") ||
		strings.HasPrefix(line, "Looking for upload port") ||
		strings.HasPrefix(line, "Forcing reset ") ||
		strings.HasPrefix(line, "Waiting for the new upload port") ||
		strings.HasPrefix(line, "Uploading "):
		events = append(events, p.startOrProgressUpload("Uploading firmware", line)...)
	}

	if strings.HasPrefix(line, "Auto-detected: ") {
		p.uploadPort = strings.TrimSpace(strings.TrimPrefix(line, "Auto-detected: "))
	}
	if strings.HasPrefix(line, "Serial port ") {
		p.uploadPort = strings.TrimSpace(strings.TrimPrefix(line, "Serial port "))
	}

	if match := sizeLineRE.FindStringSubmatch(line); match != nil {
		pct, _ := strconv.ParseFloat(match[2], 64)
		percent := pct / 100
		events = append(events, p.event(progress.PhaseSize, progress.StatusProgress, progress.LevelInfo, match[1]+" usage", line, &progress.Progress{Percent: &percent}))
	}
	if match := sizeUsedLineRE.FindStringSubmatch(line); match != nil && match[1] == "Flash" {
		p.flashUsedBytes, _ = strconv.ParseInt(match[2], 10, 64)
	}

	if match := uploadPctRE.FindStringSubmatch(line); match != nil {
		pct, _ := strconv.ParseFloat(match[1], 64)
		percent := pct / 100
		events = append(events, p.event(progress.PhaseUpload, progress.StatusProgress, progress.LevelInfo, "Writing firmware", line, &progress.Progress{Percent: &percent}))
	}

	switch {
	case strings.HasPrefix(line, dfuTargetLine):
		events = append(events, p.startOrProgressUpload("Uploading DFU package", line)...)
	case nrfHashLineRE.MatchString(line):
		events = append(events, p.nrfUploadProgress(line)...)
	case strings.HasPrefix(line, dfuActivateLine):
		events = append(events, p.uploadProgress("Activating firmware", line, 1)...)
	case strings.HasPrefix(line, dfuDoneLine):
		events = append(events, p.event(progress.PhaseUpload, progress.StatusCompleted, progress.LevelInfo, "Device programmed", line, nil))
	}

	if strings.Contains(line, "[SUCCESS]") {
		phase := progress.PhaseCompile
		if p.uploadStarted {
			phase = progress.PhaseUpload
		}
		events = append(events, p.event(phase, progress.StatusCompleted, progress.LevelInfo, "PlatformIO completed", line, nil))
	}

	return events
}

func (p *Parser) startOrProgressUpload(message, detail string) []progress.Event {
	if !p.uploadStarted {
		p.uploadStarted = true
		events := p.completeBuildPhases()
		events = append(events, p.event(progress.PhaseUpload, progress.StatusStarted, progress.LevelInfo, message, detail, nil))
		return events
	}
	return []progress.Event{p.event(progress.PhaseUpload, progress.StatusProgress, progress.LevelInfo, message, detail, nil)}
}

func (p *Parser) nrfUploadProgress(line string) []progress.Event {
	p.nrfUploadedChunks += int64(len(line))
	if p.flashUsedBytes <= 0 {
		return p.startOrProgressUpload("Uploading DFU package", line)
	}
	totalChunks := (p.flashUsedBytes + 511) / 512
	if totalChunks <= 0 {
		return p.startOrProgressUpload("Uploading DFU package", line)
	}
	percent := float64(p.nrfUploadedChunks) / float64(totalChunks)
	if percent > 1 {
		percent = 1
	}
	return p.uploadProgress("Uploading DFU package", line, percent)
}

func (p *Parser) uploadProgress(message, detail string, percent float64) []progress.Event {
	prog := &progress.Progress{Percent: &percent}
	if !p.uploadStarted {
		p.uploadStarted = true
		events := p.completeBuildPhases()
		events = append(events, p.event(progress.PhaseUpload, progress.StatusStarted, progress.LevelInfo, message, detail, prog))
		return events
	}
	return []progress.Event{p.event(progress.PhaseUpload, progress.StatusProgress, progress.LevelInfo, message, detail, prog)}
}

// Complete returns synthetic final events for phases that PlatformIO may not
// explicitly close in its output.
func (p *Parser) Complete(upload bool, logPath string) []progress.Event {
	p.mu.Lock()
	defer p.mu.Unlock()

	var events []progress.Event
	events = append(events, p.completeBuildPhases()...)
	if upload {
		message := "Flashed firmware"
		detail := logPath
		if p.uploadPort != "" {
			detail = p.uploadPort
			if logPath != "" {
				detail += " · " + logPath
			}
		}
		events = append(events, p.event(progress.PhaseUpload, progress.StatusCompleted, progress.LevelInfo, message, detail, nil))
	}
	return events
}

func (p *Parser) completeBuildPhases() []progress.Event {
	if p.buildPhasesCompleted {
		return nil
	}
	p.buildPhasesCompleted = true

	var events []progress.Event
	if p.compileStarted || p.sizeStarted || p.uploadStarted {
		events = append(events, p.event(progress.PhaseCompile, progress.StatusCompleted, progress.LevelInfo, "Compiled firmware", "", nil))
	}
	if p.linkStarted {
		events = append(events, p.event(progress.PhaseLink, progress.StatusCompleted, progress.LevelInfo, "Linked firmware", "", nil))
	} else if p.compileStarted || p.sizeStarted || p.uploadStarted {
		events = append(events, p.event(progress.PhaseLink, progress.StatusSkipped, progress.LevelInfo, "Link step not needed", "", nil))
	}
	if p.sizeStarted {
		events = append(events, p.event(progress.PhaseSize, progress.StatusCompleted, progress.LevelInfo, "Checked firmware size", "", nil))
	}
	return events
}

// UploadPort returns the last upload port parsed from PlatformIO output.
func (p *Parser) UploadPort() string {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.uploadPort
}

func (p *Parser) event(phase progress.Phase, status progress.Status, level progress.Level, message, detail string, prog *progress.Progress) progress.Event {
	return progress.Event{
		Time:     time.Now(),
		Phase:    phase,
		Status:   status,
		Level:    level,
		Message:  message,
		Detail:   detail,
		Progress: prog,
	}
}

func currentPhase(p *Parser) progress.Phase {
	switch {
	case p.uploadStarted:
		return progress.PhaseUpload
	case p.sizeStarted:
		return progress.PhaseSize
	case p.linkStarted:
		return progress.PhaseLink
	default:
		return progress.PhaseCompile
	}
}
