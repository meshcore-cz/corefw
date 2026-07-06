package cli

import (
	"encoding/json"
	"fmt"
	"io"
	"sort"

	"github.com/spf13/cobra"

	"github.com/arnal/corefw/internal/registry"
	"github.com/arnal/corefw/internal/resolve"
)

type PlanOutput struct {
	Profile      string            `json:"profile"`
	Board        ComponentOutput   `json:"board"`
	Components   []ComponentOutput `json:"components"`
	Capabilities []string          `json:"capabilities"`
	Warnings     []string          `json:"warnings,omitempty"`
	Environment  string            `json:"environment"`
}

type ComponentOutput struct {
	ID          string `json:"id"`
	Type        string `json:"type"`
	Version     string `json:"version,omitempty"`
	Status      string `json:"status,omitempty"`
	Description string `json:"description,omitempty"`
	Source      string `json:"source,omitempty"`
}

func newPlanCommand(deps commandDeps) *cobra.Command {
	var jsonOut bool
	var offline bool
	cmd := &cobra.Command{
		Use:     "plan <profile>",
		Short:   "Show the resolved firmware composition",
		GroupID: groupConfiguration,
		Args:    exactArgs(1),
		Example: `  corefw plan profiles/wio-l1.yaml
  corefw plan profiles/wio-l1.yaml --json
  corefw plan profiles/wio-l1.yaml --offline`,
		ValidArgsFunction: profileCompletion,
		RunE: func(cmd *cobra.Command, args []string) error {
			res, err := deps.PlanLoader.Load(cmd.Context(), PlanOptions{ProfilePath: args[0], Offline: offline})
			if err != nil {
				return err
			}
			out := newPlanOutput(res.Plan)
			if jsonOut {
				return writeJSON(cmd.OutOrStdout(), out)
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Profile: %s\n", out.Profile)
			fmt.Fprintf(cmd.OutOrStdout(), "Board:   %s\n\n", out.Board.ID)
			fmt.Fprintln(cmd.OutOrStdout(), "Components:")
			for _, c := range out.Components {
				fmt.Fprintf(cmd.OutOrStdout(), "  %s:%-22s %s\n", c.Type, c.ID, c.Source)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "\nCapabilities:")
			for _, cap := range out.Capabilities {
				fmt.Fprintf(cmd.OutOrStdout(), "  %s\n", cap)
			}
			fmt.Fprintln(cmd.OutOrStdout(), "\nDependencies:")
			printDependencies(cmd, res.Plan)
			fmt.Fprintln(cmd.OutOrStdout(), "\nBuild:")
			fmt.Fprintf(cmd.OutOrStdout(), "  Environment: %s\n", out.Environment)
			if res.Plan.Board.Component.Manifest.Board != nil {
				fmt.Fprintf(cmd.OutOrStdout(), "  Framework:   %s\n", res.Plan.Board.Component.Manifest.Board.PlatformIO.Framework)
			}
			for _, w := range out.Warnings {
				fmt.Fprintf(cmd.OutOrStdout(), "Warning: %s\n", w)
			}
			return nil
		},
	}
	cmd.Flags().BoolVar(&jsonOut, "json", false, "write stable JSON output")
	cmd.Flags().BoolVar(&offline, "offline", false, "use only built-in, local and cached components")
	return cmd
}

func newPlanOutput(plan *resolve.Plan) PlanOutput {
	out := PlanOutput{
		Profile:     plan.Profile.Name,
		Board:       componentOutput(plan.Board.Component),
		Environment: envName(plan.Profile.Name),
		Warnings:    append([]string{}, plan.Warnings...),
	}
	add := func(sel *resolve.Selected) {
		out.Components = append(out.Components, componentOutput(sel.Component))
	}
	add(plan.Board)
	for _, sel := range plan.Modules {
		add(sel)
	}
	for _, sel := range plan.Policies {
		add(sel)
	}
	for cap, ok := range plan.Capabilities {
		if ok {
			out.Capabilities = append(out.Capabilities, cap)
		}
	}
	sort.Strings(out.Capabilities)
	return out
}

func componentOutput(c *registry.Component) ComponentOutput {
	return ComponentOutput{
		ID:          c.Manifest.ID,
		Type:        string(c.Manifest.Type),
		Version:     c.Manifest.Version,
		Status:      string(c.Manifest.Status),
		Description: c.Manifest.Description,
		Source:      componentSource(c.Origin),
	}
}

func componentSource(origin registry.Origin) string {
	switch origin.Kind {
	case "git":
		ref := origin.Resolved
		if len(ref) > 8 {
			ref = ref[:8]
		}
		return "git · " + ref
	case "local":
		return "local · " + origin.Ref
	default:
		return "builtin"
	}
}

func printDependencies(cmd *cobra.Command, plan *resolve.Plan) {
	all := append([]*resolve.Selected{plan.Board}, plan.Modules...)
	all = append(all, plan.Policies...)
	for _, sel := range all {
		m := sel.Component.Manifest
		for _, cap := range m.Requires.Capabilities {
			fmt.Fprintf(cmd.OutOrStdout(), "  %s requires %s\n", sel.ID(), cap)
		}
		for _, svc := range m.Requires.Services {
			fmt.Fprintf(cmd.OutOrStdout(), "  %s requires service %s\n", sel.ID(), svc)
		}
		for _, dep := range m.Requires.Components {
			fmt.Fprintf(cmd.OutOrStdout(), "  %s requires component %s\n", sel.ID(), dep)
		}
	}
}

func envName(name string) string {
	out := make([]rune, 0, len(name))
	lastUnderscore := false
	for _, r := range name {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') {
			out = append(out, r)
			lastUnderscore = false
			continue
		}
		if !lastUnderscore {
			out = append(out, '_')
			lastUnderscore = true
		}
	}
	return "corefw_" + string(out)
}

func writeJSON(w io.Writer, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	_, err = fmt.Fprintln(w, string(data))
	return err
}
