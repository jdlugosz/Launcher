// Exercise the generated page and its Keep button without opening a browser.
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
class Element {
  constructor(tag) { this.tagName=tag; this.children=[]; this.attributes={}; this.listeners={}; this.dataset={}; this.style={}; this.value=''; }
  append(...items) { for(const item of items){item.parentElement=this;this.children.push(item)} }
  replaceChildren(...items) { this.children=[];this.append(...items) }
  setAttribute(name,value) { this.attributes[name]=value }
  getAttribute(name) { return this[name]??this.attributes[name] }
  addEventListener(name,fn) { this.listeners[name]=fn }
  after(...items) { const p=this.parentElement,i=p.children.indexOf(this);for(const item of items)item.parentElement=p;p.children.splice(i+1,0,...items) }
  before(...items) { const p=this.parentElement,i=p.children.indexOf(this);for(const item of items)item.parentElement=p;p.children.splice(i,0,...items) }
  closest(tag) { for(let p=this;p;p=p.parentElement)if(p.tagName===tag)return p;return null }
  querySelectorAll() { const found=[];for(const c of this.children){if(c.tagName==='a'&&c.download!==undefined)found.push(c);if(c.children)found.push(...c.querySelectorAll())}return found }
}
(async()=>{
  const html=fs.readFileSync(process.argv[2],'utf8');
  const ids=Object.fromEntries(['filter','rows','count','previous','next'].map(id=>[id,new Element(id)]));
  const root=new Element('main'),wrapper=new Element('div'),table=new Element('table');root.append(wrapper);wrapper.append(table);table.append(ids.rows);
  let observer,fail=false;const requests=[];
  const context=vm.createContext({
    document:{getElementById:id=>ids[id],createElement:tag=>new Element(tag),createTextNode:text=>({textContent:text})},
    MutationObserver:class{constructor(fn){observer=fn}observe(){}},
    fetch:async(url,options)=>{requests.push({url,...options});return{ok:!fail,json:async()=>fail?{error:'Existing file differs.'}:{path:'C:\\test\\Launcher\\Icons\\kept.svg'}}}
  });
  const scripts=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)];assert.equal(scripts.length,2);
  for(const match of scripts)vm.runInContext(match[1],context);
  const links=()=>ids.rows.querySelectorAll('a[download]');
  const keepAfter=link=>link.parentElement.children[link.parentElement.children.indexOf(link)+1];
  let first=links()[0],button=keepAfter(first);assert.equal(button.textContent,'Keep');
  await button.listeners.click();assert.equal(button.textContent,'Kept');assert.equal(button.disabled,true);
  assert.equal(requests[0].url,'keep');assert.equal(requests[0].method,'POST');assert.equal(requests[0].headers['Content-Type'],'application/json');
  assert.equal(JSON.parse(requests[0].body).asset,first.href);
  ids.filter.value='no-such-resource-xyz';ids.filter.listeners.input();observer();assert.equal(ids.rows.children.length,0);
  ids.filter.value='';ids.filter.listeners.input();observer();first=links()[0];button=keepAfter(first);assert.equal(button.textContent,'Kept');assert.equal(button.disabled,true);
  fail=true;button=keepAfter(links()[1]);await button.listeners.click();assert.equal(button.disabled,false);
  assert.ok(button.parentElement.children.some(x=>x.textContent?.includes('Existing file differs.')));
  assert.ok(!/\btitle\s*=/.test(html));
  console.log('PASS: generated gallery Keep requests, retained selection state, filtering and error recovery.');
})().catch(error=>{console.error(error);process.exitCode=1});
