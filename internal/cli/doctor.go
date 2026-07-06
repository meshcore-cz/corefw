package cli

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/spf13/cobra"

	"github.com/arnal/corefw/internal/lock"
)

type DoctorStatus string

const (
	DoctorPass DoctorStatus = "pass"
	DoctorWarn DoctorStatus = "warn"
	DoctorFail DoctorStatus = "fail"
)

type DoctorCheck struct {
	Name    string       `json:"name"`
	Status  DoctorStatus `json:"status"`
	Message string       `json:"message,omitempty"`
	Detail  string       `json:"detail,omitempty"`
}

func newDoctorCommand(deps commandDeps) *cobra.Command {
	var jsonOut bool
	cmd := &cobra.Command{
		Use:     "doctor [profile]",
		Short:   "Check the local corefw environment",
		GroupID: groupEnvironment,
		Args:    rangeArgs(0, 1),
		Example: `  corefw doctor
  corefw doctor profiles/wio-l1.yaml
  corefw doctor --json`,
		ValidArgsFunction: profileCompletion,
		RunE: func(cmd *cobra.Command, args []string) error {
			checks := runDoctorChecks(cmd.Context(), deps, args)
			if jsonOut {
				if err := writeJSON(cmd.OutOrStdout(), checks); err != nil {
					return err
				}
			} else {
				printDoctorChecks(cmd, checks)
			}
			for _, check := range checks {
				if check.Status == DoctorFail {
					return fmt.Errorf("doctor found failed checks")
				}
			}
			return nil
		},
	}
	cmd.Flags().BoolVar(&jsonOut, "json", false, "write stable JSON output")
	return cmd
}

func runDoctorChecks(ctx context.Context, deps commandDeps, args []string) []DoctorCheck {
	var checks []DoctorCheck
	checks = append(checks, DoctorCheck{Name: "corefw version", Status: DoctorPass, Message: lock.PlatformVersion})
	checks = append(checks, executableCheck(deps, "Git executable", "git"))
	checks = append(checks, executableCheck(deps, "PlatformIO executable", "pio"))
	checks = append(checks, platformIOVersionCheck(ctx, deps))
	checks = append(checks, writableDirCheck("component cache", filepath.Join(os.TempDir(), "corefw-doctor-cache")))
	checks = append(checks, writableDirCheck("build directory", "build"))
	if runtime.GOOS == "windows" || runtime.GOOS == "darwin" || runtime.GOOS == "linux" {
		checks = append(checks, DoctorCheck{Name: "supported operating system", Status: DoctorPass, Message: runtime.GOOS})
	} else {
		checks = append(checks, DoctorCheck{Name: "supported operating system", Status: DoctorWarn, Message: runtime.GOOS})
	}
	devices, err := deps.DeviceLister.List(ctx, DeviceListOptions{})
	if err != nil || len(devices) == 0 {
		checks = append(checks, DoctorCheck{Name: "serial-device access", Status: DoctorWarn, Message: "No serial device detected"})
	} else {
		checks = append(checks, DoctorCheck{Name: "serial-device access", Status: DoctorPass, Message: fmt.Sprintf("%d device(s)", len(devices))})
	}
	if len(args) == 1 {
		if res, err := deps.PlanLoader.Load(ctx, PlanOptions{ProfilePath: args[0], Offline: true}); err != nil {
			checks = append(checks, DoctorCheck{Name: "profile parsing", Status: DoctorFail, Message: err.Error()})
		} else {
			checks = append(checks, DoctorCheck{Name: "profile parsing", Status: DoctorPass, Message: args[0]})
			checks = append(checks, DoctorCheck{Name: "component resolution", Status: DoctorPass, Message: res.Plan.Board.ID()})
		}
	}
	return checks
}

func executableCheck(deps commandDeps, name, exe string) DoctorCheck {
	path, err := deps.LookPath(exe)
	if err != nil {
		return DoctorCheck{Name: name, Status: DoctorFail, Message: exe + " not found"}
	}
	return DoctorCheck{Name: name, Status: DoctorPass, Message: path}
}

func platformIOVersionCheck(ctx context.Context, deps commandDeps) DoctorCheck {
	path, err := deps.LookPath("pio")
	if err != nil {
		return DoctorCheck{Name: "PlatformIO version", Status: DoctorFail, Message: "pio not found"}
	}
	out, err := exec.CommandContext(ctx, path, "--version").Output()
	if err != nil {
		return DoctorCheck{Name: "PlatformIO version", Status: DoctorWarn, Message: err.Error()}
	}
	return DoctorCheck{Name: "PlatformIO version", Status: DoctorPass, Message: strings.TrimSpace(string(out))}
}

func writableDirCheck(name, dir string) DoctorCheck {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return DoctorCheck{Name: name, Status: DoctorFail, Message: err.Error()}
	}
	f, err := os.CreateTemp(dir, ".corefw-write-test-*")
	if err != nil {
		return DoctorCheck{Name: name, Status: DoctorFail, Message: err.Error()}
	}
	path := f.Name()
	_ = f.Close()
	_ = os.Remove(path)
	return DoctorCheck{Name: name, Status: DoctorPass, Message: "writable"}
}

func printDoctorChecks(cmd *cobra.Command, checks []DoctorCheck) {
	var warn, fail int
	for _, check := range checks {
		mark := "✓"
		if check.Status == DoctorWarn {
			mark = "⚠"
			warn++
		}
		if check.Status == DoctorFail {
			mark = "✗"
			fail++
		}
		fmt.Fprintf(cmd.OutOrStdout(), "%s %s", mark, check.Name)
		if check.Message != "" {
			fmt.Fprintf(cmd.OutOrStdout(), " %s", check.Message)
		}
		fmt.Fprintln(cmd.OutOrStdout())
	}
	fmt.Fprintf(cmd.OutOrStdout(), "\n%d warning, %d errors\n", warn, fail)
}
