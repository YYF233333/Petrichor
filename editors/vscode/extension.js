'use strict';

const fs = require('fs');
const net = require('net');
const path = require('path');
const vscode = require('vscode');
const { CloseAction, ErrorAction, LanguageClient, LSPErrorCodes, State } = require('vscode-languageclient/node');

// In ms, between attempts to connect: while no Godot editor serves the
// project, and while another client holds its server.
const RETRY = 3000;
const RETRY_REFUSED = 10000;

// As the server compares paths (architecture §2.4).
const FOLDS_CASE = process.platform === 'win32' || process.platform === 'darwin';

const projects = new Map(); // By key(root).
let output;

function key(root) {
	return FOLDS_CASE ? root.toLowerCase() : root;
}

// The project a script belongs to, as Godot finds it: the nearest folder above
// with project.godot.
function rootOf(document) {
	if (document.languageId !== 'petrichor' || document.uri.scheme !== 'file') {
		return undefined;
	}
	for (let folder = path.dirname(document.uri.fsPath); ; folder = path.dirname(folder)) {
		if (fs.existsSync(path.join(folder, 'project.godot'))) {
			return folder;
		}
		if (path.dirname(folder) === folder) {
			return undefined;
		}
	}
}

function alive(pid) {
	try {
		process.kill(pid, 0);
		return true;
	} catch (error) {
		return error.code === 'EPERM';
	}
}

// Why the server refused, from its first message.
function refusalIn(received) {
	try {
		const { error } = JSON.parse(received.slice(received.indexOf('\r\n\r\n') + 4));
		return error?.code === LSPErrorCodes.RequestFailed ? error.message : undefined;
	} catch {
		return undefined;
	}
}

function open(port) {
	return new Promise((resolve, reject) => {
		const socket = net.connect(port, '127.0.0.1', () => resolve(socket));
		socket.once('error', reject);
	});
}

// The status bar shows a lost connection, so the library pops up no error for it.
class Client extends LanguageClient {
	error(message, data, notify) {
		super.error(message, data, notify === 'force' ? false : notify);
	}
}

class Project {
	constructor(root) {
		this.root = root;
		this.name = path.basename(root);
		this.info = path.join(root, '.godot', 'petrichor', 'lsp.json');
		this.status = vscode.window.createStatusBarItem(`petrichor.${key(root)}`, vscode.StatusBarAlignment.Left);
		this.status.name = 'Petrichor';
		this.status.command = { title: 'Petrichor', command: 'petrichor.status', arguments: [key(root)] };
		// The server hears of scene and resource files the client saved, and of
		// files changed outside, only from these events (architecture §2.4).
		this.files = vscode.workspace.createFileSystemWatcher(new vscode.RelativePattern(root, '**/*.{pet,gd,tscn,scn,tres,res,godot}'));
		// A Godot editor that starts writes it.
		this.started = vscode.workspace.createFileSystemWatcher(new vscode.RelativePattern(root, '.godot/petrichor/lsp.json'), false, false, true);
		this.started.onDidCreate(() => this.connect());
		this.started.onDidChange(() => this.connect());
		this.show('waiting');
		this.connect();
	}

	async connect() {
		if (this.client || this.connecting || this.disposed) {
			return;
		}
		this.connecting = true;
		clearTimeout(this.timer);
		let socket;
		let pid;
		try {
			const info = JSON.parse(await fs.promises.readFile(this.info, 'utf8'));
			pid = info.pid;
			if (!alive(pid)) {
				throw new Error('The Godot editor that wrote lsp.json has quit.');
			}
			socket = await open(info.port);
		} catch {
			this.show('waiting');
			this.timer = setTimeout(() => this.connect(), RETRY);
			return;
		} finally {
			this.connecting = false;
		}
		if (this.disposed) {
			socket.destroy();
			return;
		}
		const client = new Client('petrichor', 'Petrichor', () => Promise.resolve({ reader: socket, writer: socket }), {
			documentSelector: [{ scheme: 'file', language: 'petrichor', pattern: { baseUri: vscode.Uri.file(this.root).toString(), pattern: '**/*.pet' } }],
			synchronize: { fileEvents: this.files },
			outputChannel: output,
			// A lost connection goes back to connecting, with a new client: the
			// server cannot know that a client which reconnects is the same.
			errorHandler: {
				error: () => ({ action: ErrorAction.Continue, handled: true }),
				closed: () => ({ action: CloseAction.DoNotRestart, handled: true, message: `The Godot editor of ${this.root} closed the connection.` }),
			},
			initializationFailedHandler: () => false,
		});
		this.client = client;
		this.socket = socket;
		client.onDidChangeState(({ newState }) => {
			if (newState === State.Stopped) {
				this.lost(client, RETRY, 'waiting');
			}
		});
		// The server refuses a second client in its reply to `initialize` and
		// closes the connection. The library handles the close before it reads
		// the reply, and drops it; so the reply is read here too.
		let received = '';
		const peek = (chunk) => (received += chunk);
		socket.on('data', peek);
		try {
			await client.start();
		} catch {
			const refusal = refusalIn(received);
			if (refusal && !this.told && this.client === client) {
				this.told = true;
				vscode.window.showWarningMessage(`${this.name}: ${refusal}`);
			}
			this.lost(client, refusal ? RETRY_REFUSED : RETRY, refusal ? 'refused' : 'waiting');
			return;
		}
		socket.off('data', peek);
		if (this.client === client) {
			this.told = false;
			output.info(`Connected to the Godot editor of ${this.root} (process ${pid}).`);
			this.show('connected');
		}
	}

	lost(client, delay, state) {
		if (this.client !== client) {
			return;
		}
		this.client = undefined;
		this.socket.destroy();
		this.show(state);
		this.timer = setTimeout(() => this.connect(), delay);
	}

	show(state) {
		const [icon, tooltip] = {
			connected: ['check', `Connected to the Godot editor of ${this.name}`],
			waiting: ['debug-disconnect', `Waiting for the Godot editor of ${this.name}`],
			refused: ['warning', `Another client is connected to the Godot editor of ${this.name}`],
		}[state];
		this.state = state;
		this.status.text = `$(${icon}) Petrichor`;
		this.status.tooltip = tooltip;
		this.status.backgroundColor = state === 'refused' ? new vscode.ThemeColor('statusBarItem.warningBackground') : undefined;
		this.reveal();
	}

	// Only beside the project's scripts.
	reveal() {
		const document = vscode.window.activeTextEditor?.document;
		const root = document && rootOf(document);
		if (root && key(root) === key(this.root)) {
			this.status.show();
		} else {
			this.status.hide();
		}
	}

	dispose() {
		this.disposed = true;
		clearTimeout(this.timer);
		this.status.dispose();
		this.files.dispose();
		this.started.dispose();
		const { client, socket } = this;
		this.client = undefined;
		return client?.dispose().catch(() => {}).finally(() => socket.destroy());
	}
}

// One project for each project.godot in the workspace, and for each above a
// script opened outside them.
async function refresh() {
	const roots = new Map();
	const add = (root) => root && roots.set(key(root), root);
	for (const file of await vscode.workspace.findFiles('**/project.godot')) {
		add(path.dirname(file.fsPath));
	}
	vscode.workspace.textDocuments.forEach((document) => add(rootOf(document)));
	for (const [each, project] of projects) {
		if (!roots.has(each)) {
			projects.delete(each);
			project.dispose();
		}
	}
	for (const [each, root] of roots) {
		if (!projects.has(each)) {
			projects.set(each, new Project(root));
		}
	}
}

function activate(context) {
	output = vscode.window.createOutputChannel('Petrichor', { log: true });
	const projectFiles = vscode.workspace.createFileSystemWatcher('**/project.godot', false, true, false);
	context.subscriptions.push(
		output,
		projectFiles,
		projectFiles.onDidCreate(refresh),
		projectFiles.onDidDelete(refresh),
		vscode.workspace.onDidChangeWorkspaceFolders(refresh),
		vscode.workspace.onDidOpenTextDocument((document) => {
			const root = rootOf(document);
			if (root && !projects.has(key(root))) {
				refresh();
			}
		}),
		vscode.window.onDidChangeActiveTextEditor(() => projects.forEach((project) => project.reveal())),
		vscode.commands.registerCommand('petrichor.status', (each) => {
			const project = projects.get(each);
			if (project?.state === 'connected') {
				output.show(true);
			} else {
				project?.connect();
			}
		}),
	);
	return refresh();
}

function deactivate() {
	return Promise.all([...projects.values()].map((project) => project.dispose()));
}

module.exports = { activate, deactivate };
