package cli

import (
	"fmt"
	"os"
	"strings"

	bubblesprogress "charm.land/bubbles/v2/progress"
	"charm.land/bubbles/v2/spinner"
	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
	"golang.org/x/term"

	"github.com/arnal/corefw/internal/build"
	"github.com/arnal/corefw/internal/progress"
)

type buildEventMsg progress.Event
type buildEventsClosedMsg struct{}

type buildDoneMsg struct {
	result *build.Result
	err    error
}

type buildUIModel struct {
	opts         build.Options
	events       chan progress.Event
	spinner      spinner.Model
	bar          bubblesprogress.Model
	status       map[progress.Phase]progress.Status
	message      map[progress.Phase]string
	detail       map[progress.Phase]string
	current      progress.Event
	done         bool
	eventsClosed bool
	result       *build.Result
	buildErr     error
	width        int
}

func runBuildUI(opts build.Options) (*build.Result, error) {
	events := make(chan progress.Event, 512)
	opts.Reporter = progress.ReporterFunc(func(event progress.Event) {
		select {
		case events <- event:
		default:
		}
	})

	program := tea.NewProgram(newBuildUIModel(opts, events), tea.WithInput(os.Stdin), tea.WithOutput(os.Stdout))
	finalModel, uiErr := program.Run()
	if model, ok := finalModel.(buildUIModel); ok {
		if model.buildErr != nil {
			return model.result, model.buildErr
		}
		if model.done {
			return model.result, nil
		}
	}

	if uiErr != nil {
		opts.Reporter = progress.NewPlainReporter(os.Stdout)
		return build.Run(opts)
	}
	return nil, fmt.Errorf("build UI exited before the build completed")
}

func newBuildUIModel(opts build.Options, events chan progress.Event) buildUIModel {
	return buildUIModel{
		opts:    opts,
		events:  events,
		spinner: spinner.New(spinner.WithSpinner(spinner.MiniDot), spinner.WithStyle(lipgloss.NewStyle().Foreground(lipgloss.Color("39")))),
		bar:     bubblesprogress.New(bubblesprogress.WithWidth(40)),
		status:  make(map[progress.Phase]progress.Status),
		message: make(map[progress.Phase]string),
		detail:  make(map[progress.Phase]string),
		width:   80,
	}
}

func (m buildUIModel) Init() tea.Cmd {
	return tea.Batch(
		func() tea.Msg { return m.spinner.Tick() },
		waitBuildEvent(m.events),
		runBuildCommand(m.opts, m.events),
	)
}

func (m buildUIModel) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	var cmds []tea.Cmd

	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.bar.SetWidth(clamp(msg.Width-28, 20, 64))

	case spinner.TickMsg:
		var cmd tea.Cmd
		m.spinner, cmd = m.spinner.Update(msg)
		if cmd != nil && !m.done {
			cmds = append(cmds, cmd)
		}

	case bubblesprogress.FrameMsg:
		var cmd tea.Cmd
		m.bar, cmd = m.bar.Update(msg)
		if cmd != nil {
			cmds = append(cmds, cmd)
		}

	case buildEventMsg:
		event := progress.Event(msg)
		m.applyEvent(event)
		cmds = append(cmds, waitBuildEvent(m.events))
		if event.Progress != nil && event.Progress.Percent != nil {
			if cmd := m.bar.SetPercent(*event.Progress.Percent); cmd != nil {
				cmds = append(cmds, cmd)
			}
		}

	case buildEventsClosedMsg:
		m.eventsClosed = true
		if m.done {
			cmds = append(cmds, tea.Quit)
		}

	case buildDoneMsg:
		m.done = true
		m.result = msg.result
		m.buildErr = msg.err
		if msg.err != nil {
			m.current = progress.Event{Phase: progress.PhaseCompile, Status: progress.StatusFailed, Level: progress.LevelError, Message: "Build failed", Detail: msg.err.Error()}
		}
		if m.eventsClosed {
			cmds = append(cmds, tea.Quit)
		}
	}

	return m, tea.Batch(cmds...)
}

func (m buildUIModel) View() tea.View {
	return tea.NewView(m.render())
}

func (m *buildUIModel) applyEvent(event progress.Event) {
	m.current = event
	if event.Phase != "" {
		m.status[event.Phase] = event.Status
	}
	if event.Message != "" {
		m.message[event.Phase] = event.Message
	}
	if event.Detail != "" {
		m.detail[event.Phase] = event.Detail
	}
}

func (m buildUIModel) render() string {
	var b strings.Builder
	title := "corefw build"
	if m.opts.Upload {
		title = "corefw flash"
	}

	fmt.Fprintln(&b, uiTitleStyle.Render(title))
	fmt.Fprintf(&b, "%s\n\n", subtleStyle.Render(m.opts.ProfilePath))

	for _, phase := range buildUIPhases(m.opts.Upload) {
		status := m.status[phase]
		label := phaseLabel(phase)
		msg := m.message[phase]
		if msg == "" {
			msg = "waiting"
		}
		fmt.Fprintf(&b, "%s %-18s %s\n", statusMark(status), label, statusStyle(status).Render(msg))
	}

	if m.current.Progress != nil && m.current.Progress.Percent != nil {
		fmt.Fprintf(&b, "\n%s\n", m.bar.ViewAs(*m.current.Progress.Percent))
	}

	current := m.current.Message
	if current == "" {
		current = "Starting"
	}
	if m.done {
		if m.buildErr != nil {
			current = "Failed: " + m.buildErr.Error()
		} else {
			current = "Complete"
		}
		fmt.Fprintf(&b, "\n%s\n", statusStyle(m.current.Status).Render(current))
	} else {
		fmt.Fprintf(&b, "\n%s %s\n", m.spinner.View(), current)
	}

	if detail := cleanDetail(m.current.Detail); detail != "" {
		fmt.Fprintf(&b, "%s\n", subtleStyle.Render(truncate(detail, max(40, m.width-4))))
	}

	return b.String()
}

func waitBuildEvent(events <-chan progress.Event) tea.Cmd {
	return func() tea.Msg {
		event, ok := <-events
		if !ok {
			return buildEventsClosedMsg{}
		}
		return buildEventMsg(event)
	}
}

func runBuildCommand(opts build.Options, events chan progress.Event) tea.Cmd {
	return func() tea.Msg {
		defer close(events)
		result, err := build.Run(opts)
		return buildDoneMsg{result: result, err: err}
	}
}

func useInteractiveBuildUI() bool {
	return isInteractiveBuildOutput(int(os.Stdout.Fd()), os.Getenv)
}

func isInteractiveBuildOutput(fd int, getenv func(string) string) bool {
	if getenv("CI") != "" {
		return false
	}
	if getenv("TERM") == "dumb" {
		return false
	}
	return fd >= 0 && term.IsTerminal(fd)
}

func buildUIPhases(upload bool) []progress.Phase {
	phases := []progress.Phase{
		progress.PhaseLoadProfile,
		progress.PhaseFetchComponents,
		progress.PhaseResolveComponents,
		progress.PhaseGenerateProject,
		progress.PhaseWriteLockfile,
		progress.PhasePrepareToolchain,
		progress.PhaseCompile,
		progress.PhaseLink,
		progress.PhaseSize,
	}
	if upload {
		phases = append(phases, progress.PhaseUpload)
	}
	return phases
}

func phaseLabel(phase progress.Phase) string {
	switch phase {
	case progress.PhaseLoadProfile:
		return "profile"
	case progress.PhaseFetchComponents:
		return "components"
	case progress.PhaseResolveComponents:
		return "resolve"
	case progress.PhaseGenerateProject:
		return "generate"
	case progress.PhaseWriteLockfile:
		return "lockfile"
	case progress.PhasePrepareToolchain:
		return "toolchain"
	case progress.PhaseCompile:
		return "compile"
	case progress.PhaseLink:
		return "link"
	case progress.PhaseSize:
		return "size"
	case progress.PhaseUpload:
		return "upload"
	default:
		return string(phase)
	}
}

func statusMark(status progress.Status) string {
	switch status {
	case progress.StatusStarted, progress.StatusProgress:
		return "[>]"
	case progress.StatusCompleted:
		return "[x]"
	case progress.StatusSkipped:
		return "[-]"
	case progress.StatusWarning:
		return "[!]"
	case progress.StatusFailed:
		return "[!]"
	default:
		return "[ ]"
	}
}

func statusStyle(status progress.Status) lipgloss.Style {
	switch status {
	case progress.StatusCompleted:
		return lipgloss.NewStyle().Foreground(lipgloss.Color("42"))
	case progress.StatusSkipped:
		return subtleStyle
	case progress.StatusWarning:
		return lipgloss.NewStyle().Foreground(lipgloss.Color("214"))
	case progress.StatusFailed:
		return lipgloss.NewStyle().Foreground(lipgloss.Color("196"))
	case progress.StatusStarted, progress.StatusProgress:
		return lipgloss.NewStyle().Foreground(lipgloss.Color("39"))
	default:
		return subtleStyle
	}
}

var (
	uiTitleStyle = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("39"))
	subtleStyle  = lipgloss.NewStyle().Foreground(lipgloss.Color("245"))
)

func cleanDetail(detail string) string {
	return strings.Join(strings.Fields(detail), " ")
}

func truncate(s string, maxLen int) string {
	if maxLen <= 0 || len(s) <= maxLen {
		return s
	}
	if maxLen <= 3 {
		return s[:maxLen]
	}
	return s[:maxLen-3] + "..."
}

func clamp(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}
