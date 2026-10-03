//go:build unix

package mcpapp

import (
	"os"
	"syscall"
)

func tryLock(f *os.File) error {
	return syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB)
}
