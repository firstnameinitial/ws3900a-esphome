// SPDX-License-Identifier: GPL-3.0-only
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('components/ecowitt_rf/sensor/dashboard.html', 'utf8');
const elements = new Map();
const element = id => {
  if (!elements.has(id)) elements.set(id, {id, value:'', checked:false, disabled:false});
  return elements.get(id);
};
const context = vm.createContext({
  document: {addEventListener() {}, activeElement:null, getElementById:element},
  Date, encodeURIComponent, console,
});
vm.runInContext(source.match(/<script>([\s\S]*?)<\/script>/)[1], context);
const run = code => vm.runInContext(code, context);
const plain = code => JSON.parse(run(`JSON.stringify(${code})`));
const posts = [];
context.fetch = async (url, options) => {
  assert.equal(url, '/config');
  assert.equal(options.method, 'POST');
  assert.equal(options.headers['Content-Type'], 'application/x-www-form-urlencoded');
  const form = new URLSearchParams(options.body);
  assert.deepEqual([...form.keys()], ['j']);
  posts.push(JSON.parse(form.get('j')));
  return {ok:true,json:async()=>({ok:true})};
};
(async () => {
  // The options must not silently claim support on firmware without cfg.panel.
  run('S={cfg:{up:{en:true},tz:"UTC"}};renderPanelOptions()');
  assert.equal(element('panelSave').disabled,true);
  assert.equal(element('pn_on').disabled,true);
  await run('savePanel()');assert.equal(posts.length,0);
  assert.equal(element('panelSaveStat').textContent,'Unavailable');
  // A new status snapshot can add support without reloading the page.
  run('S.cfg.panel={};panelDirty=false;renderPanelOptions()');
  assert.equal(element('panelSave').disabled,false);
  assert.equal(element('pn_on').value,'07:00');
  assert.equal(element('pn_off').value,'22:00');
  assert.equal(element('pn_week').value,'0');
  assert.equal(element('pn_season').value,'1');
  element('pn_schedule').checked=true;element('pn_on').value='00:00';element('pn_off').value='23:59';
  element('pn_hilo').checked=true;element('pn_day').value='23';element('pn_week').value='1';
  element('pn_season').value='12';element('pn_piezo').value='0';
  await run('savePanel()');
  assert.deepEqual(posts[0],{panel:{schedule:{enabled:true,on_minute:0,off_minute:1439},
    rain:{day_hour:23,week_monday:true,season_month:12,prefer_piezo:false},daily_hilo:true}});
  assert.deepEqual(Object.keys(posts[0]),['panel']);
  assert.equal(plain('S.cfg.up').en,true);
  assert.equal(element('panelSaveStat').textContent,'saved');
  // Refreshes retain unsaved input instead of restoring the last polled value.
  element('pn_on').value='00:01';run('panelDirty=true;renderPanelOptions()');
  assert.equal(element('pn_on').value,'00:01');
  // Validate before POST; HTML time constraints alone do not run for a plain button.
  for (const time of ['24:00','12:60','7:00','','23:59:00']) {
    const count=posts.length, old=plain('S.cfg.panel');element('pn_on').value=time;
    await run('savePanel()');assert.equal(posts.length,count);assert.deepEqual(plain('S.cfg.panel'),old);
    assert.match(element('panelSaveStat').textContent,/Invalid/);
  }
  element('pn_on').value='22:00';element('pn_off').value='07:00';
  for (const [id,value] of [['pn_day','24'],['pn_day','-1'],['pn_season','0'],['pn_season','13'],['pn_day','1.5'],['pn_week','true'],['pn_piezo','']]) {
    const prior=element(id).value,count=posts.length;element(id).value=value;
    await run('savePanel()');assert.equal(posts.length,count);element(id).value=prior;
  }
  // Overnight schedules and lower rain boundaries remain valid.
  element('pn_day').value='0';element('pn_week').value='0';element('pn_season').value='1';element('pn_piezo').value='1';
  element('pn_schedule').checked=false;element('pn_hilo').checked=false;
  await run('savePanel()');assert.deepEqual(posts.at(-1),{panel:{schedule:{enabled:false,on_minute:1320,off_minute:420},
    rain:{day_hour:0,week_monday:false,season_month:1,prefer_piezo:true},daily_hilo:false}});
  // Existing upload/system saves neither send the console form nor delete its state.
  const priorPanel=plain('S.cfg.panel');run('collectCfg=()=>({up:{en:false}})');
  await run('save("up")');assert.deepEqual(posts.at(-1),{up:{en:false}});
  assert.deepEqual(plain('S.cfg.panel'),priorPanel);
  context.fetch=async()=>({ok:false,status:503,json:async()=>({ok:false,err:'busy'})});
  element('pn_hilo').checked=true;await run('savePanel()');
  assert.deepEqual(plain('S.cfg.panel'),priorPanel);assert.equal(element('panelSaveStat').textContent,'busy');
  assert.equal(element('panelSave').disabled,false);
  // Actual server echoes take precedence, and firmware downgrade disables the card.
  context.fetch=async()=>({ok:true,json:async()=>({ok:true,cfg:{up:{en:false},panel:{schedule:{enabled:true,on_minute:60,off_minute:120},rain:{day_hour:1,week_monday:true,season_month:2,prefer_piezo:true},daily_hilo:false}}})});
  await run('savePanel()');assert.equal(element('pn_on').value,'01:00');assert.equal(element('pn_day').value,'1');
  run('delete S.cfg.panel;renderPanelOptions()');assert.equal(element('panelSave').disabled,true);
  assert.match(source, /id="pn_on"[^>]*type="time"/);
  assert.match(source, /id="pn_day"[^>]*><option value="0">00:00<\/option>/);
  console.log('Console settings: real j-form payloads, day/time/month bounds, overnight schedule, old firmware, unsaved edits and rejected saves passed');
})().catch(error => {console.error(error);process.exitCode=1;});
