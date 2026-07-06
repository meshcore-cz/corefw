package cli

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/arnal/corefw/internal/lock"
)

func newLockCommand(deps commandDeps) *cobra.Command {
	cmd := &cobra.Command{
		Use:     "lock",
		Short:   "Inspect corefw lock data",
		GroupID: groupConfiguration,
		Args: func(cmd *cobra.Command, args []string) error {
			if len(args) == 1 {
				return nil
			}
			if len(args) == 0 {
				return cmd.Help()
			}
			return usageErrorf("lock accepts a profile or subcommand")
		},
		RunE: func(cmd *cobra.Command, args []string) error {
			if len(args) == 1 {
				return runLockShow(cmd, deps, args[0], true)
			}
			return nil
		},
	}
	cmd.AddCommand(newLockShowCommand(deps))
	return cmd
}

func newLockShowCommand(deps commandDeps) *cobra.Command {
	var offline bool
	cmd := &cobra.Command{
		Use:               "show <profile>",
		Short:             "Print the resolved lockfile JSON",
		Args:              exactArgs(1),
		ValidArgsFunction: profileCompletion,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runLockShow(cmd, deps, args[0], offline)
		},
	}
	cmd.Flags().BoolVar(&offline, "offline", false, "do not access Git remotes")
	return cmd
}

func runLockShow(cmd *cobra.Command, deps commandDeps, profilePath string, offline bool) error {
	res, err := deps.PlanLoader.Load(cmd.Context(), PlanOptions{ProfilePath: profilePath, Offline: offline})
	if err != nil {
		return err
	}
	data, err := jsonIndent(lock.Build(res.Plan))
	if err != nil {
		return err
	}
	fmt.Fprintln(cmd.OutOrStdout(), string(data))
	return nil
}
