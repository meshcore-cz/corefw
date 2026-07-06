package cli

import "github.com/spf13/cobra"

func newCompletionCommand(deps commandDeps) *cobra.Command {
	cmd := &cobra.Command{
		Use:     "completion",
		Short:   "Generate shell completion scripts",
		GroupID: groupOther,
		Args:    noArgs,
	}
	cmd.AddCommand(&cobra.Command{
		Use:   "bash",
		Short: "Generate Bash completion",
		Args:  noArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			return cmd.Root().GenBashCompletion(cmd.OutOrStdout())
		},
	})
	cmd.AddCommand(&cobra.Command{
		Use:   "zsh",
		Short: "Generate Zsh completion",
		Args:  noArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			return cmd.Root().GenZshCompletion(cmd.OutOrStdout())
		},
	})
	cmd.AddCommand(&cobra.Command{
		Use:   "fish",
		Short: "Generate Fish completion",
		Args:  noArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			return cmd.Root().GenFishCompletion(cmd.OutOrStdout(), true)
		},
	})
	cmd.AddCommand(&cobra.Command{
		Use:   "powershell",
		Short: "Generate PowerShell completion",
		Args:  noArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			return cmd.Root().GenPowerShellCompletion(cmd.OutOrStdout())
		},
	})
	return cmd
}
