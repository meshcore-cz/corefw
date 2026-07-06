package cli

import (
	"context"
	"errors"
	"fmt"
)

// ExecuteContext runs corefw with production dependencies and returns a process
// exit code.
func ExecuteContext(ctx context.Context, args []string) int {
	return ExecuteContextWithDependencies(ctx, DefaultDependencies(), args)
}

// ExecuteContextWithDependencies runs corefw with injected dependencies.
func ExecuteContextWithDependencies(ctx context.Context, deps Dependencies, args []string) int {
	cmd := NewRootCommand(deps)
	cmd.SetArgs(args)
	cmd.SetContext(ctx)
	err := cmd.Execute()
	if err == nil {
		return 0
	}
	if errors.Is(ctx.Err(), context.Canceled) || errors.Is(err, context.Canceled) {
		fmt.Fprintln(cmd.ErrOrStderr(), "interrupted")
		return 130
	}
	fmt.Fprintf(cmd.ErrOrStderr(), "error: %v\n", err)
	if isUsageError(err) {
		return 2
	}
	return 1
}
