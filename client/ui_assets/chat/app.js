let isChatOpen = false;

function set_chatbox_open(open) {
    isChatOpen = open;
    const wrapper = document.getElementById('chat-input-wrapper');
    const input = document.getElementById('chat-input');

    if (open) {
        wrapper.classList.remove('hidden');
        input.focus();
    } else {
        wrapper.classList.add('hidden');
        input.blur();
    }
}

function send_chat_message(text) {
    const container = document.getElementById('chat-messages');
    const msgEl = document.createElement('div');
    msgEl.className = 'chat-msg';

    // Parse color codes estilo SA-MP / FiveM (^1, ^2, ^3...)
    let formatted = text
        .replace(/\^1/g, '<span style="color: #e74c3c;">')
        .replace(/\^2/g, '<span style="color: #2ecc71;">')
        .replace(/\^3/g, '<span style="color: #f1c40f;">')
        .replace(/\^4/g, '<span style="color: #3498db;">')
        .replace(/\^5/g, '<span style="color: #9b59b6;">');

    msgEl.innerHTML = formatted;
    container.appendChild(msgEl);

    if (container.children.length > 25) {
        container.removeChild(container.firstChild);
    }
}

document.getElementById('chat-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
        const val = e.target.value.trim();
        if (val.length > 0 && window.frontier) {
            window.frontier.sendChatMessage(val);
        }
        e.target.value = '';
        set_chatbox_open(false);
    } else if (e.key === 'Escape') {
        set_chatbox_open(false);
    }
});
