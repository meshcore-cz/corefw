package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/spf13/cobra"

	"github.com/arnal/corefw/internal/lock"
)

const (
	groupFirmware      = "firmware"
	groupConfiguration = "configuration"
	groupEnvironment   = "environment"
	groupOther         = "other"
)

type rootOptions struct {
	Plain   bool
	Verbose bool
	NoColor bool
}

// NewRootCommand creates the corefw command hierarchy.
func NewRootCommand(deps Dependencies) *cobra.Command {
	deps = deps.withDefaults()
	var rootOpts rootOptions

	cmd := &cobra.Command{
		Use:   "corefw",
		Short: "Compose, build, test and flash modular MeshCore firmware",
		Long: `corefw composes MeshCore firmware from declarative YAML profiles.
It resolves and validates board, module and policy components, then generates a
statically composed PlatformIO firmware project.`,
		SilenceErrors: true,
		SilenceUsage:  true,
		Version:       lock.PlatformVersion,
		RunE: func(cmd *cobra.Command, args []string) error {
			return cmd.Help()
		},
	}
	cmd.SetVersionTemplate("corefw {{.Version}}\n")
	cmd.SetOut(deps.Stdout)
	cmd.SetErr(deps.Stderr)
	cmd.SetIn(deps.Stdin)
	cmd.SetFlagErrorFunc(func(cmd *cobra.Command, err error) error {
		return &UsageError{Err: err}
	})
	cmd.PersistentFlags().BoolVar(&rootOpts.Plain, "plain", false, "disable interactive UI and decorative output")
	cmd.PersistentFlags().BoolVar(&rootOpts.Verbose, "verbose", false, "show detailed internal and PlatformIO output")
	cmd.PersistentFlags().BoolVar(&rootOpts.NoColor, "no-color", false, "disable color in output")

	cobra.AddTemplateFunc("cmdGroups", func() []*cobra.Group {
		return cmd.Groups()
	})
	cmd.AddGroup(
		&cobra.Group{ID: groupFirmware, Title: "Firmware"},
		&cobra.Group{ID: groupConfiguration, Title: "Configuration"},
		&cobra.Group{ID: groupEnvironment, Title: "Environment"},
		&cobra.Group{ID: groupOther, Title: "Other"},
	)

	commandDeps := commandDeps{Dependencies: deps, Root: &rootOpts}
	cmd.AddCommand(
		newPrepareCommand(commandDeps),
		newBuildCommand(commandDeps),
		newFlashCommand(commandDeps),
		newMonitorCommand(commandDeps),
		newCleanCommand(commandDeps),
		newValidateCommand(commandDeps),
		newPlanCommand(commandDeps),
		newComponentCommand(commandDeps),
		newLockCommand(commandDeps),
		newDoctorCommand(commandDeps),
		newDevicesCommand(commandDeps),
		newCompletionCommand(commandDeps),
		newVersionCommand(commandDeps),
		newLegacyComponentsCommand(commandDeps),
		newLegacyBoardsCommand(commandDeps),
	)
	return cmd
}

type commandDeps struct {
	Dependencies
	Root *rootOptions
}

func exactArgs(n int) cobra.PositionalArgs {
	return func(cmd *cobra.Command, args []string) error {
		if len(args) != n {
			return usageErrorf("%s accepts %d arg(s), received %d", cmd.CommandPath(), n, len(args))
		}
		return nil
	}
}

func rangeArgs(min, max int) cobra.PositionalArgs {
	return func(cmd *cobra.Command, args []string) error {
		if len(args) < min || len(args) > max {
			return usageErrorf("%s accepts between %d and %d arg(s), received %d", cmd.CommandPath(), min, max, len(args))
		}
		return nil
	}
}

func noArgs(cmd *cobra.Command, args []string) error {
	if len(args) != 0 {
		return usageErrorf("%s accepts no arguments", cmd.CommandPath())
	}
	return nil
}

func profileCompletion(cmd *cobra.Command, args []string, toComplete string) ([]string, cobra.ShellCompDirective) {
	return builtinProfileNames(toComplete), cobra.ShellCompDirectiveDefault
}

func builtinProfileNames(prefix string) []string {
	entries, err := os.ReadDir(findProfilesDir())
	if err != nil {
		return nil
	}
	var names []string
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		ext := filepath.Ext(entry.Name())
		if ext != ".yaml" && ext != ".yml" {
			continue
		}
		name := strings.TrimSuffix(entry.Name(), ext)
		if prefix == "" || strings.HasPrefix(name, prefix) {
			names = append(names, name)
		}
	}
	return names
}

func outputError(cmd *cobra.Command, err error) error {
	if err == nil {
		return nil
	}
	return fmt.Errorf("%w", err)
}
