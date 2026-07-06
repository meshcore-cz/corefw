package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"text/tabwriter"

	"github.com/spf13/cobra"

	"github.com/arnal/corefw/internal/manifest"
	"github.com/arnal/corefw/internal/registry"
)

func newComponentCommand(deps commandDeps) *cobra.Command {
	cmd := &cobra.Command{
		Use:     "component",
		Short:   "Inspect and validate components",
		GroupID: groupConfiguration,
		Args:    noArgs,
	}
	cmd.AddCommand(
		newComponentListCommand(deps),
		newComponentShowCommand(deps),
		newComponentValidateCommand(deps),
	)
	return cmd
}

func newComponentListCommand(deps commandDeps) *cobra.Command {
	var kind string
	var status string
	var jsonOut bool
	cmd := &cobra.Command{
		Use:   "list",
		Short: "List available components",
		Args:  noArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runComponentList(cmd, kind, status, jsonOut)
		},
	}
	cmd.Flags().StringVar(&kind, "type", "", "filter by component type")
	cmd.Flags().StringVar(&status, "status", "", "filter by component status")
	cmd.Flags().BoolVar(&jsonOut, "json", false, "write stable JSON output")
	return cmd
}

func runComponentList(cmd *cobra.Command, kind, status string, jsonOut bool) error {
	reg, err := registry.New()
	if err != nil {
		return err
	}
	var comps []ComponentOutput
	for _, c := range reg.All() {
		if kind != "" && string(c.Manifest.Type) != kind {
			continue
		}
		if status != "" && string(c.Manifest.Status) != status {
			continue
		}
		comps = append(comps, componentOutput(c))
	}
	if jsonOut {
		return writeJSON(cmd.OutOrStdout(), comps)
	}
	w := tabwriter.NewWriter(cmd.OutOrStdout(), 0, 2, 2, ' ', 0)
	fmt.Fprintln(w, "ID\tTYPE\tVERSION\tSTATUS\tDESCRIPTION")
	for _, c := range comps {
		fmt.Fprintf(w, "%s\t%s\t%s\t%s\t%s\n", c.ID, c.Type, c.Version, c.Status, c.Description)
	}
	return w.Flush()
}

func newComponentShowCommand(deps commandDeps) *cobra.Command {
	var jsonOut bool
	cmd := &cobra.Command{
		Use:   "show <component-id>",
		Short: "Show component details",
		Args:  exactArgs(1),
		ValidArgsFunction: func(cmd *cobra.Command, args []string, toComplete string) ([]string, cobra.ShellCompDirective) {
			reg, err := registry.New()
			if err != nil {
				return nil, cobra.ShellCompDirectiveNoFileComp
			}
			var ids []string
			for _, c := range reg.All() {
				ids = append(ids, c.ID())
			}
			return ids, cobra.ShellCompDirectiveNoFileComp
		},
		RunE: func(cmd *cobra.Command, args []string) error {
			reg, err := registry.New()
			if err != nil {
				return err
			}
			comp, ok := reg.Get(args[0])
			if !ok {
				return fmt.Errorf("component %q not found; run `corefw component list`", args[0])
			}
			if jsonOut {
				return writeJSON(cmd.OutOrStdout(), componentShowOutput(comp))
			}
			printComponent(cmd, comp)
			return nil
		},
	}
	cmd.Flags().BoolVar(&jsonOut, "json", false, "write stable JSON output")
	return cmd
}

func newComponentValidateCommand(deps commandDeps) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "validate <component-path>",
		Short: "Validate a component package or manifest",
		Args:  exactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			dir := args[0]
			if filepath.Base(dir) == "component.yaml" {
				dir = filepath.Dir(dir)
			}
			comp, err := registry.LoadDir(dir, registry.Origin{Kind: "local", Ref: dir})
			if err != nil {
				return err
			}
			if !validKind(comp.Manifest.Type) {
				return fmt.Errorf("unsupported component type %q", comp.Manifest.Type)
			}
			for _, rel := range comp.Manifest.Build.SrcFilter {
				if strings.HasPrefix(rel, "+<") && strings.HasSuffix(rel, ">") {
					p := strings.TrimSuffix(strings.TrimPrefix(rel, "+<"), ">")
					if _, err := os.Stat(filepath.Join(dir, p)); err != nil {
						return fmt.Errorf("referenced source path %s: %w", p, err)
					}
				}
			}
			fmt.Fprintf(cmd.OutOrStdout(), "✓ %s is valid\n", comp.ID())
			return nil
		},
	}
	return cmd
}

func validKind(kind manifest.Kind) bool {
	switch kind {
	case manifest.KindBoard, manifest.KindModule, manifest.KindPolicy, manifest.KindDriver, manifest.KindApp:
		return true
	default:
		return false
	}
}

func componentShowOutput(c *registry.Component) map[string]any {
	m := c.Manifest
	return map[string]any{
		"id":                    m.ID,
		"type":                  m.Type,
		"version":               m.Version,
		"status":                m.Status,
		"description":           m.Description,
		"source":                componentSource(c.Origin),
		"requires":              m.Requires,
		"provides":              m.Provides,
		"conflicts":             m.Conflicts,
		"platformio":            m.Board,
		"option_schema_present": c.Schema.Properties != nil,
	}
}

func printComponent(cmd *cobra.Command, c *registry.Component) {
	m := c.Manifest
	fmt.Fprintf(cmd.OutOrStdout(), "ID:          %s\n", m.ID)
	fmt.Fprintf(cmd.OutOrStdout(), "Type:        %s\n", m.Type)
	fmt.Fprintf(cmd.OutOrStdout(), "Version:     %s\n", m.Version)
	fmt.Fprintf(cmd.OutOrStdout(), "Status:      %s\n", m.Status)
	fmt.Fprintf(cmd.OutOrStdout(), "Source:      %s\n", componentSource(c.Origin))
	fmt.Fprintf(cmd.OutOrStdout(), "Description: %s\n", m.Description)
	fmt.Fprintf(cmd.OutOrStdout(), "Provides:    %s\n", strings.Join(m.Provides, ", "))
	fmt.Fprintf(cmd.OutOrStdout(), "Conflicts:   %s\n", strings.Join(m.Conflicts, ", "))
	fmt.Fprintf(cmd.OutOrStdout(), "Requires:\n")
	for _, cap := range m.Requires.Capabilities {
		fmt.Fprintf(cmd.OutOrStdout(), "  capability %s\n", cap)
	}
	for _, svc := range m.Requires.Services {
		fmt.Fprintf(cmd.OutOrStdout(), "  service %s\n", svc)
	}
	for _, dep := range m.Requires.Components {
		fmt.Fprintf(cmd.OutOrStdout(), "  component %s\n", dep)
	}
	if m.Board != nil {
		fmt.Fprintf(cmd.OutOrStdout(), "PlatformIO:\n")
		fmt.Fprintf(cmd.OutOrStdout(), "  board:     %s\n", m.Board.PlatformIO.Board)
		fmt.Fprintf(cmd.OutOrStdout(), "  framework: %s\n", m.Board.PlatformIO.Framework)
		fmt.Fprintf(cmd.OutOrStdout(), "  platform:  %s\n", m.Board.PlatformIO.Platform)
	}
}

func newLegacyComponentsCommand(deps commandDeps) *cobra.Command {
	return &cobra.Command{
		Use:        "components",
		Short:      "List available components",
		Hidden:     true,
		Deprecated: "use `corefw component list`",
		Args:       noArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runComponentList(cmd, "", "", false)
		},
	}
}

func newLegacyBoardsCommand(deps commandDeps) *cobra.Command {
	return &cobra.Command{
		Use:        "boards",
		Short:      "List available board packages",
		Hidden:     true,
		Deprecated: "use `corefw component list --type board`",
		Args:       noArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runComponentList(cmd, string(manifest.KindBoard), "", false)
		},
	}
}
