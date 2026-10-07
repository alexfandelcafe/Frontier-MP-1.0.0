document.addEventListener('DOMContentLoaded', () => {
    // Manejo de pestañas
    const tabs = document.querySelectorAll('.tab-btn');
    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            tabs.forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));
            tab.classList.add('active');
            const target = tab.getAttribute('data-tab');
            document.getElementById(`tab-${target}`).classList.add('active');
        });
    });

    // Botón de conexión
    document.getElementById('btn-connect').addEventListener('click', () => {
        const playerName = document.getElementById('player-name').value.trim();
        const address = document.getElementById('server-address').value.trim();

        const parts = address.split(':');
        const host = parts[0] || '127.0.0.1';
        const port = parseInt(parts[1] || '4674', 10);

        console.log(`Connecting to ${host}:${port} as ${playerName}...`);

        if (window.frontier) {
            window.frontier.connect(host, port, playerName);
        } else {
            console.log("CEF Environment: window.frontier not yet bound.");
        }
    });
});

function quickConnect(address) {
    document.getElementById('server-address').value = address;
    document.getElementById('btn-connect').click();
}
