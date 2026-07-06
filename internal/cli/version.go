package cli

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/arnal/corefw/internal/lock"
)

func newVersionCommand(deps commandDeps) *cobra.Command {
	return &cobra.Command{
		Use:     "version",
		Short:   "Print the corefw version",
		GroupID: groupOther,
		Args:    noArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			fmt.Fprintf(cmd.OutOrStdout(), "corefw %s\n", lock.PlatformVersion)
			return nil
		},
	}
}
