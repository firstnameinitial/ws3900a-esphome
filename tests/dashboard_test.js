// SPDX-License-Identifier: GPL-3.0-only
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('components/ecowitt_rf/sensor/dashboard.html', 'utf8');
const elements = new Map();
const context = vm.createContext({
  document: {addEventListener() {}, getElementById(id) {
    if (!elements.has(id)) elements.set(id, {});
    return elements.get(id);
  }},
  Date, encodeURIComponent, console,
});
vm.runInContext(source.match(/<script>([\s\S]*?)<\/script>/)[1], context);
const run = code => vm.runInContext(code, context);
(async () => {
  assert.match(run("miniVal({rain_state:1},'rain_state','Wet flag','',0)"), /WET/);
  assert.doesNotMatch(run("miniVal({rain_state:1},'rain_state','Wet flag','',0)"), />RAIN</);
  // Busy response must retain readings and avoid creating fictitious demo state on first poll.
  context.fetch = async () => ({status:503, ok:false});
  await run('load()'); assert.equal(run('S'), null);
  run('S={dev:{name:"real"},r:{rain_24h:1.2}}; online=true; lastFetchAt=Date.now(); demoMode=false;');
  await run('load()'); assert.equal(run('S.r.rain_24h'), 1.2); assert.equal(run('online'), true);
  run('lastFetchAt=Date.now()-20000');
  await run('load()'); assert.equal(run('online'), false); assert.equal(run('demoMode'), false);
  // Exercise both real UI save paths against the IDF j-form parser's contract.
  run('renderRoster=()=>{};collectCfg=()=>({up:{en:false,key:"****1234"}});S.roster=[{fam:"WS90",id:"000001",en:true}];');
  const posts = [];
  context.fetch = async (url, options) => {
    assert.equal(options.headers['Content-Type'], 'application/x-www-form-urlencoded');
    posts.push(JSON.parse(new URLSearchParams(options.body).get('j')));
    return {ok:true,json:async()=>({ok:true})};
  };
  await run('postEnable("WS90","000001",false)');
  assert.equal(posts[0].sensors[0].en, false);
  await run('save("up")'); assert.equal(posts[1].up.key, '****1234');
  context.fetch=async()=>({ok:false,json:async()=>({ok:false,err:'busy'})});
  await run('postEnable("WS90","000001",true)');
  assert.equal(run('S.roster[0].en'), false); // rejected changes do not appear saved
  assert.equal(elements.get('upSaveStat').textContent, 'saved');
  console.log('Dashboard wet flag, busy polling and both IDF form saves passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
