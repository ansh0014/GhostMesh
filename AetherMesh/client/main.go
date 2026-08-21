package main

import (
	"crypto/sha256"
	"flag"
	"fmt"
	"net"
	"os"
	"os/signal"
	"syscall"

	"aethermesh/client/gossip"
	"aethermesh/client/ui"

	tea "github.com/charmbracelet/bubbletea"
)

func main() {
	name := flag.String("name", "peer", "Your display name in the chat")
	psk := flag.String("psk", "", "Pre-shared key (must match all other nodes)")
	daemonSocket := flag.String("socket", "/tmp/aether.sock", "Path to daemon Unix socket")
	flag.Parse()

	if *psk == "" {
		fmt.Fprintln(os.Stderr, "Error: --psk is required")
		os.Exit(1)
	}

	// Derive 32-byte key from PSK using SHA-256
	keyHash := sha256.Sum256([]byte(*psk))
	key := keyHash[:]

	// Connect to the daemon via Unix socket
	conn, err := net.Dial("unix", *daemonSocket)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: Cannot connect to daemon at %s\n", *daemonSocket)
		fmt.Fprintln(os.Stderr, "Make sure aetherd is running first.")
		os.Exit(1)
	}
	defer conn.Close()

	// Send the derived key to the daemon as a handshake
	_, err = conn.Write(key)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: Handshake with daemon failed: %v\n", err)
		os.Exit(1)
	}

	// Setup gossip router
	router := gossip.NewRouter(*name, conn)

	// Start listening for incoming messages from daemon in background
	go router.Listen()

	// Setup graceful shutdown
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigChan
		// Zero out key from memory before exit
		for i := range key {
			key[i] = 0
		}
		conn.Close()
		os.Exit(0)
	}()

	// Launch Bubble Tea TUI
	model := ui.NewModel(*name, router, router.Messages())
	p := tea.NewProgram(
		model,
		tea.WithAltScreen(),
		tea.WithMouseCellMotion(),
	)

	if _, err := p.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "TUI error: %v\n", err)
		os.Exit(1)
	}
}
