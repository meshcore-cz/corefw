package cli

import (
	"errors"
	"fmt"
	"text/tabwriter"

	"github.com/spf13/cobra"

	"github.com/arnal/corefw/internal/platformio"
)

type DeviceListOptions struct {
	All bool
}

type DeviceOutput struct {
	Port        string `json:"port"`
	Description string `json:"description,omitempty"`
	HardwareID  string `json:"hardware_id,omitempty"`
	BoardHint   string `json:"board_hint,omitempty"`
}

func newDevicesCommand(deps commandDeps) *cobra.Command {
	var jsonOut bool
	var all bool
	cmd := &cobra.Command{
		Use:     "devices",
		Short:   "List connected serial devices",
		GroupID: groupEnvironment,
		Args:    noArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			devices, err := deps.DeviceLister.List(cmd.Context(), DeviceListOptions{All: all})
			if errors.Is(err, platformio.ErrNotFound) {
				return fmt.Errorf("PlatformIO was not found. Run `corefw doctor` for setup details")
			}
			if err != nil {
				return err
			}
			if jsonOut {
				return writeJSON(cmd.OutOrStdout(), devices)
			}
			w := tabwriter.NewWriter(cmd.OutOrStdout(), 0, 2, 2, ' ', 0)
			fmt.Fprintln(w, "PORT\tDESCRIPTION\tHARDWARE")
			for _, d := range devices {
				fmt.Fprintf(w, "%s\t%s\t%s\n", d.Port, d.Description, d.HardwareID)
			}
			return w.Flush()
		},
	}
	cmd.Flags().BoolVar(&jsonOut, "json", false, "write stable JSON output")
	cmd.Flags().BoolVar(&all, "all", false, "include all device classes reported by PlatformIO")
	return cmd
}
