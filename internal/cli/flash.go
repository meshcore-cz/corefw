package cli

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/arnal/corefw/internal/build"
)

type flashFlags struct {
	outputDir string
	firmware  string
	port      string
	monitor   bool
}

func newFlashCommand(deps commandDeps) *cobra.Command {
	var flags flashFlags
	cmd := &cobra.Command{
		Use:     "flash <profile>",
		Short:   "Build and upload firmware to a device",
		GroupID: groupFirmware,
		Args:    exactArgs(1),
		Example: `  corefw flash profiles/wio-l1.yaml
  corefw flash profiles/wio-l1.yaml --port /dev/ttyACM0
  corefw flash profiles/wio-l1.yaml --monitor`,
		ValidArgsFunction: profileCompletion,
		RunE: func(cmd *cobra.Command, args []string) error {
			profilePath := resolveProfileArg(args[0])
			res, err := runBuildCommand(cmd, deps, build.Options{
				Context:     cmd.Context(),
				ProfilePath: profilePath,
				OutDir:      flags.outputDir,
				FirmwareDir: flags.firmware,
				Upload:      true,
				Port:        flags.port,
			})
			if err != nil {
				return err
			}
			reportGenerated(cmd, res.OutDir, res.Gen.Files)
			reportFlashResult(cmd, res)
			reportPIOLog(cmd, res)
			if flags.monitor {
				if res.PIOLog == "" {
					return fmt.Errorf("upload was skipped; not starting monitor")
				}
				return deps.MonitorRunner.Run(cmd.Context(), MonitorOptions{
					Port:    flags.port,
					Baud:    115200,
					Project: res.OutDir,
					Env:     res.Gen.EnvName,
				})
			}
			return nil
		},
	}
	cmd.Flags().StringVar(&flags.outputDir, "out", "", "output directory (default build/<name>)")
	cmd.Flags().StringVar(&flags.firmware, "firmware", "firmware", "path to the corefw C++ firmware tree")
	cmd.Flags().StringVar(&flags.port, "port", "", "upload/serial port (e.g. /dev/ttyUSB0); autodetected if omitted")
	cmd.Flags().BoolVar(&flags.monitor, "monitor", false, "start PlatformIO monitor after a successful upload")
	return cmd
}

func reportFlashResult(cmd *cobra.Command, res *build.Result) {
	if res == nil {
		return
	}
	if res.UploadPort != "" {
		fmt.Fprintf(cmd.OutOrStdout(), "\nFlashed firmware to %s\n", res.UploadPort)
		return
	}
	if res.PIOLog != "" {
		fmt.Fprintln(cmd.OutOrStdout(), "\nFlashed firmware")
	}
}
