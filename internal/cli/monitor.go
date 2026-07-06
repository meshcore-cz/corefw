package cli

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/spf13/cobra"
)

type MonitorOptions struct {
	Port    string
	Baud    int
	Filters []string
	Project string
	Env     string
}

func newMonitorCommand(deps commandDeps) *cobra.Command {
	var opts MonitorOptions
	cmd := &cobra.Command{
		Use:     "monitor [profile]",
		Short:   "Open a PlatformIO serial monitor",
		GroupID: groupFirmware,
		Args:    rangeArgs(0, 1),
		Example: `  corefw monitor
  corefw monitor --port /dev/ttyACM0
  corefw monitor profiles/wio-l1.yaml --baud 9600`,
		ValidArgsFunction: profileCompletion,
		RunE: func(cmd *cobra.Command, args []string) error {
			if len(args) == 1 && opts.Project == "" {
				res, err := deps.PlanLoader.Load(cmd.Context(), PlanOptions{ProfilePath: args[0], Offline: true})
				if err != nil {
					return err
				}
				outDir := filepath.Join("build", res.Profile.Name)
				if _, err := os.Stat(filepath.Join(outDir, "platformio.ini")); err != nil {
					return fmt.Errorf("generated project not found at %s; run `corefw build %s --no-compile` first", outDir, args[0])
				}
				opts.Project = outDir
				opts.Env = envName(res.Profile.Name)
			}
			return deps.MonitorRunner.Run(cmd.Context(), opts)
		},
	}
	cmd.Flags().StringVar(&opts.Port, "port", "", "serial port")
	cmd.Flags().IntVar(&opts.Baud, "baud", 115200, "serial baud rate")
	cmd.Flags().StringArrayVar(&opts.Filters, "filter", nil, "PlatformIO monitor filter (repeatable)")
	cmd.Flags().StringVar(&opts.Project, "project", "", "generated PlatformIO project directory")
	cmd.Flags().StringVar(&opts.Env, "env", "", "PlatformIO environment")
	return cmd
}
