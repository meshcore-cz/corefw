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
	sizeLineRE  = regexp.MustCompile(`^(RAM|Flash):\s+\[[^\]]*\]\s+([0-9]+(?:\.[0-9]+)?)%`)
	uploadPctRE = regexp.MustCompile(`\(([0-9]+)\s*%\)`)
)

// Parser converts PlatformIO output lines into structured progress events.
type Parser struct {
	mu             sync.Mutex
	compileStarted bool
	linkStarted    bool
	sizeStarted    bool
	uploadStarted  bool
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
		strings.HasPrefix(line, "Uploading "):
		if !p.uploadStarted {
			p.uploadStarted = true
			events = append(events, p.event(progress.PhaseUpload, progress.StatusStarted, progress.LevelInfo, "Uploading firmware", line, nil))
		} else {
			events = append(events, p.event(progress.PhaseUpload, progress.StatusProgress, progress.LevelInfo, "Uploading firmware", line, nil))
		}
	}

	if match := sizeLineRE.FindStringSubmatch(line); match != nil {
		pct, _ := strconv.ParseFloat(match[2], 64)
		percent := pct / 100
		events = append(events, p.event(progress.PhaseSize, progress.StatusProgress, progress.LevelInfo, match[1]+" usage", line, &progress.Progress{Percent: &percent}))
	}

	if match := uploadPctRE.FindStringSubmatch(line); match != nil {
		pct, _ := strconv.ParseFloat(match[1], 64)
		percent := pct / 100
		events = append(events, p.event(progress.PhaseUpload, progress.StatusProgress, progress.LevelInfo, "Writing firmware", line, &progress.Progress{Percent: &percent}))
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
