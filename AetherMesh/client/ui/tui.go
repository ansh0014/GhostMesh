package ui

import (
	"fmt"
	"strings"
	"time"

	"aethermesh/client/gossip"

	"github.com/charmbracelet/bubbles/textinput"
	"github.com/charmbracelet/bubbles/viewport"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

// Styles
var (
	headerStyle = lipgloss.NewStyle().
			Background(lipgloss.Color("#1a1a2e")).
			Foreground(lipgloss.Color("#00d4ff")).
			Bold(true).
			Padding(0, 1)

	peerCountStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#7bed9f")).
			Bold(true)

	dividerStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#2d2d2d"))

	myMsgStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#00d4ff"))

	peerMsgStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#ffffff"))

	timeStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#555555"))

	senderStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#bd93f9")).
			Bold(true)

	hintStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#444444")).
			Italic(true)

	inputPromptStyle = lipgloss.NewStyle().
				Foreground(lipgloss.Color("#00d4ff")).
				Bold(true)
)

type DisplayMessage struct {
	Sender    string
	Text      string
	Timestamp time.Time
	IsOwn     bool
}

// newMessageCmd wraps an incoming gossip.Message as a tea.Cmd
type newMessageMsg gossip.Message
type peerCountMsg int

func WaitForMessage(ch <-chan gossip.Message) tea.Cmd {
	return func() tea.Msg {
		msg, ok := <-ch
		if !ok {
			return nil
		}
		return newMessageMsg(msg)
	}
}

// Model is the Bubble Tea application model
type Model struct {
	viewport  viewport.Model
	textInput textinput.Model
	messages  []DisplayMessage
	peerCount int
	myName    string
	msgChan   <-chan gossip.Message
	router    *gossip.Router
	width     int
	height    int
	ready     bool
}

func NewModel(name string, router *gossip.Router, msgChan <-chan gossip.Message) Model {
	ti := textinput.New()
	ti.Placeholder = "Type a message..."
	ti.Focus()
	ti.CharLimit = 512
	ti.Prompt = inputPromptStyle.Render("You ❯ ")

	return Model{
		textInput: ti,
		myName:    name,
		msgChan:   msgChan,
		router:    router,
	}
}

func (m Model) Init() tea.Cmd {
	return tea.Batch(
		textinput.Blink,
		WaitForMessage(m.msgChan),
	)
}

func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	var cmds []tea.Cmd

	switch msg := msg.(type) {

	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		headerHeight := 3
		footerHeight := 3
		vpHeight := m.height - headerHeight - footerHeight
		if !m.ready {
			m.viewport = viewport.New(m.width, vpHeight)
			m.viewport.SetContent("")
			m.ready = true
		} else {
			m.viewport.Width = m.width
			m.viewport.Height = vpHeight
		}

	case newMessageMsg:
		gm := gossip.Message(msg)
		dm := DisplayMessage{
			Sender:    gm.Sender,
			Text:      gm.Text,
			Timestamp: gm.Timestamp,
			IsOwn:     gm.Sender == m.myName,
		}
		m.messages = append(m.messages, dm)
		m.viewport.SetContent(m.renderMessages())
		m.viewport.GotoBottom()
		cmds = append(cmds, WaitForMessage(m.msgChan))

	case peerCountMsg:
		m.peerCount = int(msg)

	case tea.KeyMsg:
		switch msg.String() {

		case "ctrl+c", "/quit":
			return m, tea.Quit

		case "enter":
			input := strings.TrimSpace(m.textInput.Value())
			if input == "" {
				break
			}

			switch input {
			case "/help":
				m.messages = append(m.messages, DisplayMessage{
					Sender:    "system",
					Text:      "Commands: /help  /peers  /quit",
					Timestamp: time.Now(),
				})
			case "/peers":
				m.messages = append(m.messages, DisplayMessage{
					Sender:    "system",
					Text:      fmt.Sprintf("Active peers: %d", m.peerCount),
					Timestamp: time.Now(),
				})
			default:
				// Send message
				dm := DisplayMessage{
					Sender:    m.myName,
					Text:      input,
					Timestamp: time.Now(),
					IsOwn:     true,
				}
				m.messages = append(m.messages, dm)
				go m.router.Send(input)
			}

			m.viewport.SetContent(m.renderMessages())
			m.viewport.GotoBottom()
			m.textInput.SetValue("")
		}
	}

	var vpCmd tea.Cmd
	m.viewport, vpCmd = m.viewport.Update(msg)
	cmds = append(cmds, vpCmd)

	var tiCmd tea.Cmd
	m.textInput, tiCmd = m.textInput.Update(msg)
	cmds = append(cmds, tiCmd)

	return m, tea.Batch(cmds...)
}

func (m Model) View() string {
	if !m.ready {
		return "Initializing AetherMesh..."
	}

	// Header
	title := "  AetherMesh — Ephemeral Offline Chat"
	peers := peerCountStyle.Render(fmt.Sprintf("peers: %d  ", m.peerCount))
	padding := m.width - lipgloss.Width(title) - lipgloss.Width(peers)
	if padding < 0 {
		padding = 0
	}
	header := headerStyle.Width(m.width).Render(title + strings.Repeat(" ", padding) + peers)

	// Divider
	divider := dividerStyle.Render(strings.Repeat("─", m.width))

	// Footer: input + hints
	hints := hintStyle.Render("  /help  /peers  /quit")
	footer := fmt.Sprintf("%s\n%s", m.textInput.View(), hints)

	return fmt.Sprintf("%s\n%s\n%s\n%s", header, m.viewport.View(), divider, footer)
}

func (m Model) renderMessages() string {
	var sb strings.Builder
	for _, msg := range m.messages {
		ts := timeStyle.Render(fmt.Sprintf("[%s]", msg.Timestamp.Format("15:04:05")))
		sender := senderStyle.Render(msg.Sender)

		var text string
		if msg.IsOwn {
			text = myMsgStyle.Render(msg.Text)
		} else {
			text = peerMsgStyle.Render(msg.Text)
		}

		sb.WriteString(fmt.Sprintf("%s  %s  →  %s\n", ts, sender, text))
	}
	return sb.String()
}
