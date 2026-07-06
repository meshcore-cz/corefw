package cli

import (
	"bytes"
	"context"
	"errors"
	"path/filepath"
	"strings"
	"testing"

	"github.com/arnal/corefw/internal/build"
	"github.com/arnal/corefw/internal/codegen"
	"github.com/arnal/corefw/internal/manifest"
	"github.com/arnal/corefw/internal/profile"
	"github.com/arnal/corefw/internal/registry"
	"github.com/arnal/corefw/internal/resolve"
)

func executeCommand(t *testing.T, deps Dependencies, args ...string) (string, string, error) {
	t.Helper()
	out := new(bytes.Buffer)
	errOut := new(bytes.Buffer)
	deps.Stdout = out
	deps.Stderr = errOut
	deps.Stdin = strings.NewReader("")
	cmd := NewRootCommand(deps)
	cmd.SetArgs(args)
	cmd.SetOut(out)
	cmd.SetErr(errOut)
	cmd.SetIn(deps.Stdin)
	err := cmd.Execute()
	return out.String(), errOut.String(), err
}

func testDeps() Dependencies {
	return Dependencies{
		Getenv:           func(string) string { return "" },
		LookPath:         func(string) (string, error) { return "/bin/tool", nil },
		WorkingDirectory: func() (string, error) { return "/tmp", nil },
		IsStdoutTerminal: func() bool { return false },
		PlanLoader: PlanLoaderFunc(func(ctx context.Context, opts PlanOptions) (*PlanResult, error) {
			return fakePlan(opts.ProfilePath), nil
		}),
		DeviceLister: DeviceListerFunc(func(ctx context.Context, opts DeviceListOptions) ([]DeviceOutput, error) {
			return []DeviceOutput{{Port: "/dev/ttyUSB0", Description: "Wio Tracker L1", HardwareID: "USB VID:PID=1234"}}, nil
		}),
		MonitorRunner: MonitorRunnerFunc(func(context.Context, MonitorOptions) error { return nil }),
	}
}

func TestRootHelpAndVersion(t *testing.T) {
	stdout, _, err := executeCommand(t, testDeps())
	if err != nil {
		t.Fatalf("root help: %v", err)
	}
	if !strings.Contains(stdout, "Firmware") {
		t.Fatalf("help missing groups:\n%s", stdout)
	}

	stdout, _, err = executeCommand(t, testDeps(), "--version")
	if err != nil {
		t.Fatalf("--version: %v", err)
	}
	if strings.TrimSpace(stdout) != "corefw 0.1.0" {
		t.Fatalf("version output = %q", stdout)
	}
}

func TestUnknownCommandIsUsageError(t *testing.T) {
	_, _, err := executeCommand(t, testDeps(), "wat")
	if err == nil || !isUsageError(err) {
		t.Fatalf("expected usage error, got %v", err)
	}
}

func TestBuildReceivesFlags(t *testing.T) {
	var got build.Options
	deps := testDeps()
	deps.BuildRunner = BuildRunnerFunc(func(ctx context.Context, opts build.Options) (*build.Result, error) {
		got = opts
		return &build.Result{OutDir: opts.OutDir, Gen: &codegen.Result{EnvName: "env", Files: []string{"platformio.ini"}}}, nil
	})
	_, _, err := executeCommand(t, deps, "build", "profile.yaml", "--out", "build/custom", "--firmware", "fw", "--no-compile")
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	if got.ProfilePath != "profile.yaml" || got.OutDir != "build/custom" || got.FirmwareDir != "fw" || got.Compile {
		t.Fatalf("unexpected build opts: %+v", got)
	}
}

func TestPrepareReceivesFlags(t *testing.T) {
	var got build.Options
	deps := testDeps()
	deps.BuildRunner = BuildRunnerFunc(func(ctx context.Context, opts build.Options) (*build.Result, error) {
		got = opts
		return &build.Result{OutDir: opts.OutDir, Gen: &codegen.Result{EnvName: "env", Files: []string{"platformio.ini"}}}, nil
	})
	stdout, _, err := executeCommand(t, deps, "prepare", "profile.yaml", "--out", "build/custom", "--firmware", "fw")
	if err != nil {
		t.Fatalf("prepare: %v", err)
	}
	if got.ProfilePath != "profile.yaml" || got.OutDir != "build/custom" || got.FirmwareDir != "fw" || got.Compile || got.Upload {
		t.Fatalf("unexpected prepare opts: %+v", got)
	}
	if !strings.Contains(stdout, "Build with: pio run -e env -d build/custom") {
		t.Fatalf("prepare output = %s", stdout)
	}
}

func TestBuildReportsArtifacts(t *testing.T) {
	deps := testDeps()
	deps.BuildRunner = BuildRunnerFunc(func(ctx context.Context, opts build.Options) (*build.Result, error) {
		return &build.Result{
			OutDir:    "build/x",
			Gen:       &codegen.Result{EnvName: "env", Files: []string{"platformio.ini"}},
			Artifacts: []string{"build/x/.pio/build/env/firmware.bin", "build/x/.pio/build/env/firmware.elf"},
		}, nil
	})
	stdout, _, err := executeCommand(t, deps, "build", "profile.yaml")
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	if !strings.Contains(stdout, "Built firmware artifacts") || !strings.Contains(stdout, "firmware.bin") {
		t.Fatalf("build output = %s", stdout)
	}
}

func TestBuildResolvesBuiltinProfileName(t *testing.T) {
	var got build.Options
	deps := testDeps()
	deps.BuildRunner = BuildRunnerFunc(func(ctx context.Context, opts build.Options) (*build.Result, error) {
		got = opts
		return &build.Result{OutDir: "build/heltec-v3-repeater", Gen: &codegen.Result{EnvName: "env", Files: []string{"platformio.ini"}}}, nil
	})
	_, _, err := executeCommand(t, deps, "build", "heltec-v3-repeater", "--no-compile")
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	want := filepath.Join(findProfilesDir(), "heltec-v3-repeater.yaml")
	if got.ProfilePath != want {
		t.Fatalf("profile path = %q", got.ProfilePath)
	}
}

func TestResolveProfileArg(t *testing.T) {
	want := filepath.Join(findProfilesDir(), "heltec-v3-repeater.yaml")
	if got := resolveProfileArg("heltec-v3-repeater"); got != want {
		t.Fatalf("resolveProfileArg = %q", got)
	}
	if got := resolveProfileArg("profiles/heltec-v3-repeater.yaml"); got != "profiles/heltec-v3-repeater.yaml" {
		t.Fatalf("explicit path changed to %q", got)
	}
	if got := resolveProfileArg("does-not-exist"); got != "does-not-exist" {
		t.Fatalf("missing profile changed to %q", got)
	}
}

func TestFlashReceivesPortAndMonitorRuns(t *testing.T) {
	var gotBuild build.Options
	var gotMonitor MonitorOptions
	deps := testDeps()
	deps.BuildRunner = BuildRunnerFunc(func(ctx context.Context, opts build.Options) (*build.Result, error) {
		gotBuild = opts
		return &build.Result{OutDir: "build/x", PIOLog: "build/x/platformio.log", Gen: &codegen.Result{EnvName: "env", Files: []string{"platformio.ini"}}}, nil
	})
	deps.MonitorRunner = MonitorRunnerFunc(func(ctx context.Context, opts MonitorOptions) error {
		gotMonitor = opts
		return nil
	})
	_, _, err := executeCommand(t, deps, "flash", "profile.yaml", "--port", "/dev/ttyACM0", "--monitor")
	if err != nil {
		t.Fatalf("flash: %v", err)
	}
	if !gotBuild.Upload || gotBuild.Port != "/dev/ttyACM0" {
		t.Fatalf("unexpected flash build opts: %+v", gotBuild)
	}
	if gotMonitor.Port != "/dev/ttyACM0" || gotMonitor.Project != "build/x" || gotMonitor.Env != "env" {
		t.Fatalf("unexpected monitor opts: %+v", gotMonitor)
	}
}

func TestValidateQuietAndPlanJSON(t *testing.T) {
	stdout, _, err := executeCommand(t, testDeps(), "validate", "profile.yaml", "--quiet")
	if err != nil {
		t.Fatalf("validate: %v", err)
	}
	if stdout != "" {
		t.Fatalf("quiet validate wrote %q", stdout)
	}

	stdout, _, err = executeCommand(t, testDeps(), "plan", "profile.yaml", "--json", "--offline")
	if err != nil {
		t.Fatalf("plan: %v", err)
	}
	if !strings.Contains(stdout, `"profile": "profile"`) {
		t.Fatalf("plan json = %s", stdout)
	}
}

func TestDevicesAndMissingPlatformIO(t *testing.T) {
	stdout, _, err := executeCommand(t, testDeps(), "devices")
	if err != nil {
		t.Fatalf("devices: %v", err)
	}
	if !strings.Contains(stdout, "/dev/ttyUSB0") {
		t.Fatalf("devices output = %s", stdout)
	}

	deps := testDeps()
	deps.DeviceLister = DeviceListerFunc(func(context.Context, DeviceListOptions) ([]DeviceOutput, error) {
		return nil, errors.New("PlatformIO was not found. Run `corefw doctor` for setup details")
	})
	_, _, err = executeCommand(t, deps, "devices")
	if err == nil {
		t.Fatalf("expected devices error")
	}
}

func TestLegacyCommands(t *testing.T) {
	stdout, _, err := executeCommand(t, testDeps(), "boards")
	if err != nil {
		t.Fatalf("boards: %v", err)
	}
	if !strings.Contains(stdout, "heltec-v3") {
		t.Fatalf("boards output = %s", stdout)
	}

	stdout, _, err = executeCommand(t, testDeps(), "lock", "profile.yaml")
	if err != nil {
		t.Fatalf("legacy lock: %v", err)
	}
	if !strings.Contains(stdout, `"profile": "profile"`) {
		t.Fatalf("lock output = %s", stdout)
	}
}

func fakePlan(path string) *PlanResult {
	p := &profile.Profile{Name: strings.TrimSuffix(path, ".yaml"), Board: "heltec-v3"}
	if p.Name == "profile" || p.Name == "" {
		p.Name = "profile"
	}
	board := &registry.Component{Manifest: manifest.Manifest{
		ID:      "heltec-v3",
		Type:    manifest.KindBoard,
		Version: "1.0.0",
		Status:  manifest.StatusOfficial,
		Board: &manifest.BoardSpec{PlatformIO: manifest.PlatformIOBoard{
			Board:     "heltec_wifi_lora_32_V3",
			Framework: "arduino",
		}, Capabilities: map[string]bool{"radio": true}},
	}, Origin: registry.Origin{Kind: "builtin"}}
	module := &registry.Component{Manifest: manifest.Manifest{
		ID:      "companion",
		Type:    manifest.KindModule,
		Version: "1.0.0",
		Status:  manifest.StatusOfficial,
		Requires: manifest.Requirements{
			Capabilities: []string{"radio"},
		},
	}, Origin: registry.Origin{Kind: "builtin"}}
	plan := &resolve.Plan{
		Profile:      p,
		Board:        &resolve.Selected{Component: board},
		Modules:      []*resolve.Selected{{Component: module}},
		Capabilities: map[string]bool{"radio": true},
	}
	return &PlanResult{Profile: p, Plan: plan}
}
