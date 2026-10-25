const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

// Exercise the actual script extracted from the built DLL, not a duplicated JS fixture.
const script = fs.readFileSync(process.argv[2], 'utf8');
const ID = 'example.hello-hubcap';
function setup(hostOptions = {}) {
    let onResult, nextId = 0;
    const entries = [], states = [], sends = [], timers = new Map();
    const host = {
        version: 1,
        setManageMenuEntry: (...args) => entries.push(args),
        onResult: (id, callback) => { assert.equal(id, ID); onResult = callback; },
        showConfirm: (options, callback) => { host.confirm = { options, callback }; },
        setModalState: (...args) => states.push(args),
        send: (...args) => { sends.push(args); return `request-${++nextId}`; },
        ...hostOptions
    };
    const context = vm.createContext({
        HubcapPluginHost: host,
        setTimeout: callback => { const id = timers.size + 1; timers.set(id, callback); return id; },
        clearTimeout: id => timers.delete(id)
    });
    vm.runInContext(script, context);
    return { host, context, entries, states, sends, timers,
        result: (response, id = `request-${nextId}`) => onResult(response, { replyTo: id }) };
}

vm.runInNewContext(script, {}); // Older or absent bridge must be harmless.
assert.equal(setup({ version: 2 }).entries.length, 0);
assert.equal(setup({ send: undefined }).entries.length, 0);
assert.equal(setup({ setManageMenuEntry: undefined }).entries.length, 0, 'Missing setManageMenuEntry must be a harmless no-op, not a throw');
const ui = setup();
assert.equal(ui.entries.length, 1);
assert.equal(ui.entries[0][0], ID);
assert.equal(ui.entries[0][1].label, 'Hello Hubcap');
vm.runInContext(script, ui.context);
assert.equal(ui.entries.length, 1, 'Reinjection must not duplicate the registered entry');

ui.entries[0][1].onActivate({}); // No appId: must not open a dialog.
assert.equal(ui.host.confirm, undefined);

ui.entries[0][1].onActivate({ appId: '123' });
assert.equal(ui.host.confirm.options.confirmLabel, 'Say hello');
const modal = {};
assert.equal(ui.host.confirm.callback(modal), false);
assert.equal(ui.states.at(-1)[1].phase, 'busy');
assert.equal(ui.sends[0][0], ID);
assert.equal(ui.sends[0][1], 'hello');
assert.equal(ui.sends[0][2].appId, '123');

ui.entries[0][1].onActivate({ appId: '456' }); // Already pending for 123: must not start a second request.
assert.equal(ui.sends.length, 1);

ui.result({ ok: true, state: { message: 'Background state' } }, 'unrelated');
assert.equal(ui.states.at(-1)[1].phase, 'busy', 'Ignore unrelated background replies');
ui.result({ ok: true, kind: 'hello', state: { message: 'Hello from a native HubcapUI plugin.' } });
assert.equal(ui.states.at(-1)[1].phase, 'success');
assert.equal(ui.states.at(-1)[1].message, 'Hello from a native HubcapUI plugin.');
assert.equal(ui.timers.size, 0);

ui.entries[0][1].onActivate({ appId: '123' });
ui.host.confirm.callback(modal);
ui.result({ ok: false, error: { message: 'Native failure' } });
assert.equal(ui.states.at(-1)[1].phase, 'error');
assert.equal(ui.states.at(-1)[1].message, 'Native failure');

ui.entries[0][1].onActivate({ appId: '123' });
ui.host.confirm.callback(modal);
Array.from(ui.timers.values())[0]();
assert.equal(ui.states.at(-1)[1].title, 'Plugin request timed out');
const beforeLateReply = ui.states.length;
ui.result({ ok: true });
assert.equal(ui.states.length, beforeLateReply, 'A reply after its own timeout must be ignored');

console.log('Plugin template UI checks passed (guards, registration, activation, replies, errors, timeout).');
