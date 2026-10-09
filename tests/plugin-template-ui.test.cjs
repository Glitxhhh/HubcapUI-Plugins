const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

// Exercise the actual script extracted from the built DLL, not a duplicated JS fixture.
const script = fs.readFileSync(process.argv[2], 'utf8');
const ID = 'example.hello-hubcap';
function setup(hostOptions = {}) {
    let app = '123', onResult, interval, nextId = 0;
    const settings = [], states = [], sends = [], timers = new Map();
    const host = {
        version: 1,
        libraryAppId: () => app,
        setGameSetting: (...args) => settings.push(args),
        onResult: (id, callback) => { assert.equal(id, ID); onResult = callback; },
        showConfirm: (options, callback) => { host.confirm = { options, callback }; },
        setModalState: (...args) => states.push(args),
        send: (...args) => { sends.push(args); return `request-${++nextId}`; },
        ...hostOptions
    };
    const context = vm.createContext({
        HubcapPluginHost: host,
        setInterval: callback => { interval = callback; },
        setTimeout: callback => { const id = timers.size + 1; timers.set(id, callback); return id; },
        clearTimeout: id => timers.delete(id)
    });
    vm.runInContext(script, context);
    return { host, context, settings, states, sends, timers,
        changeApp: value => { app = value; interval(); },
        result: (response, id = `request-${nextId}`) => onResult(response, { replyTo: id }) };
}

vm.runInNewContext(script, {}); // Older or absent bridge must be harmless.
assert.equal(setup({ version: 2 }).settings.length, 0);
assert.equal(setup({ send: undefined }).settings.length, 0);
const ui = setup();
assert.equal(ui.settings.length, 1);
assert.deepEqual(ui.settings[0].slice(0, 2), [ID, '123']);
assert.equal(ui.settings[0][2].label, 'Hello Hubcap');
vm.runInContext(script, ui.context);
assert.equal(ui.settings.length, 1, 'Reinjection must not duplicate settings or timers');

ui.settings[0][2].onActivate();
assert.equal(ui.host.confirm.options.confirmLabel, 'Say hello');
const modal = {};
assert.equal(ui.host.confirm.callback(modal), false);
assert.equal(ui.states.at(-1)[1].phase, 'busy');
assert.equal(ui.sends[0][0], ID);
assert.equal(ui.sends[0][1], 'hello');
assert.equal(ui.sends[0][2].appId, '123');
ui.result({ ok: true, state: { message: 'Background state' } }, 'unrelated');
assert.equal(ui.states.at(-1)[1].phase, 'busy', 'Ignore unrelated background replies');
ui.result({ ok: true, kind: 'hello', state: { message: 'Hello from a native HubcapUI plugin.' } });
assert.equal(ui.states.at(-1)[1].phase, 'success');
assert.equal(ui.states.at(-1)[1].message, 'Hello from a native HubcapUI plugin.');
assert.equal(ui.timers.size, 0);

ui.settings[0][2].onActivate();
ui.host.confirm.callback(modal);
ui.result({ ok: false, error: { message: 'Native failure' } });
assert.equal(ui.states.at(-1)[1].phase, 'error');
assert.equal(ui.states.at(-1)[1].message, 'Native failure');
ui.settings[0][2].onActivate();
ui.host.confirm.callback(modal);
Array.from(ui.timers.values())[0]();
assert.equal(ui.states.at(-1)[1].title, 'Plugin request timed out');
const beforeLateReply = ui.states.length;
ui.result({ ok: true });
assert.equal(ui.states.length, beforeLateReply);

ui.changeApp('456');
assert.deepEqual(ui.settings.at(-2), [ID, '123', null]);
assert.equal(ui.settings.at(-1)[1], '456');
ui.changeApp('');
assert.deepEqual(ui.settings.at(-1), [ID, '456', null]);
console.log('Plugin template UI checks passed (guards, routing, replies, errors, timeout).');
