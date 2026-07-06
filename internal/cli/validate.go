package cli

import (
	"fmt"

	"github.com/spf13/cobra"
)

func newValidateCommand(deps commandDeps) *cobra.Command {
	var quiet bool
	cmd := &cobra.Command{
		Use:     "validate <profile>",
		Short:   "Validate a profile and component graph",
		GroupID: groupConfiguration,
		Args:    exactArgs(1),
		Example: `  corefw validate profiles/heltec-v3-repeater.yaml
  corefw validate profiles/heltec-v3-repeater.yaml --quiet`,
		ValidArgsFunction: profileCompletion,
		RunE: func(cmd *cobra.Command, args []string) error {
			plan, err := deps.PlanLoader.Load(cmd.Context(), PlanOptions{ProfilePath: args[0], Offline: true})
			if err != nil {
				return err
			}
			if quiet {
				return nil
			}
			fmt.Fprintf(cmd.OutOrStdout(), "✓ %s is valid\n", args[0])
			fmt.Fprintf(cmd.OutOrStdout(), "  board:    %s\n", plan.Plan.Board.ID())
			fmt.Fprintf(cmd.OutOrStdout(), "  modules:  %s\n", ids(plan.Plan.Modules))
			fmt.Fprintf(cmd.OutOrStdout(), "  policies: %s\n", policyIDs(plan.Plan.Policies))
			for _, w := range plan.Plan.Warnings {
				fmt.Fprintf(cmd.OutOrStdout(), "  warning:  %s\n", w)
			}
			return nil
		},
	}
	cmd.Flags().BoolVar(&quiet, "quiet", false, "print nothing on success")
	return cmd
}
