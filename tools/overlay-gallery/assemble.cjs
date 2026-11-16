// Assemble self-contained documentation from production-rendered PNG samples.
// No image library, network connection or browser package is required.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const [nativeDirectory, outputDirectory] = process.argv.slice(2);
if (!nativeDirectory || !outputDirectory) throw new Error('Usage: node assemble.cjs native-samples new-output-directory');
const manifest = JSON.parse(fs.readFileSync(path.join(nativeDirectory, 'manifest.json'), 'utf8'));
const images = {};
const hashes = {};
for (const name of fs.readdirSync(nativeDirectory).filter(name => name.endsWith('.png'))) {
  const bytes = fs.readFileSync(path.join(nativeDirectory, name));
  images[name] = 'data:image/png;base64,' + bytes.toString('base64');
  hashes[name] = crypto.createHash('sha256').update(bytes).digest('hex');
}
for (const sample of manifest.samples) {
  const suffix = `${sample.size_dip}-${sample.scale_percent}`;
  for (const source of manifest.icon_sources) {
    for (const state of ['ready', 'pending']) {
      if (!images[`${source}-${suffix}-${state}.png`]) throw new Error('Incomplete native sample set: ' + suffix);
    }
  }
}
const sourceHashes = Object.fromEntries([4, 5, 6].map(slats => {
  const file = `pending-blinds-${slats}.svg`;
  const bytes = fs.readFileSync(path.join(__dirname, '../../resources', file));
  return [file, crypto.createHash('sha256').update(bytes).digest('hex')];
}));

// Also runs in the offline HTML page. Icons are embedded native PNGs; SVG text,
// panels and labels remain editable in the manual illustration.
function makeSheet(images, source = 'folder', scale = 200) {
  const names = {folder: 'Folder', document: 'Document', world: 'World'};
  const sizes = [16, 24, 32, 40, 48, 64, 80, 96, 128];
  const slats = size => size <= 32 ? 4 : size <= 64 ? 5 : 6;
  const image = (icon, size, state, x, y) => `<image x="${x}" y="${y}" width="${size}" height="${size}" href="${images[`${icon}-${size}-${scale}-${state}.png`]}"/>`;
  const text = (x, y, content, size = 19, fill = '#435767', weight = 400, extra = '') => `<text x="${x}" y="${y}" font-size="${size}" fill="${fill}" font-weight="${weight}" ${extra}>${content}</text>`;
  const cards = sizes.map((size, index) => {
    const x = 60 + (index % 3) * 368;
    const y = 180 + Math.floor(index / 3) * 252;
    return `<rect x="${x}" y="${y}" width="344" height="228" rx="16" fill="#f1f5f6" stroke="#d9e3e8"/>
      ${text(x + 20, y + 33, `${size} DIP`, 23, '#183647', 650)}
      ${text(x + 323, y + 32, `${slats(size)} slats`, 17, '#526875', 400, 'text-anchor="end"')}
      ${image(source, size, 'ready', x + 91 - size / 2, y + 118 - size / 2)}
      ${image(source, size, 'pending', x + 253 - size / 2, y + 118 - size / 2)}
      ${text(x + 91, y + 205, 'Normal', 17, '#526875', 400, 'text-anchor="middle"')}
      ${text(x + 253, y + 205, 'Checking', 17, '#183647', 600, 'text-anchor="middle"')}`;
  }).join('');
  const backgrounds = [
    {name: 'Light panel', fill: '#edf3f5', ink: '#183647'},
    {name: 'Warm panel', fill: '#f3c9b8', ink: '#183647'},
    {name: 'Dark panel', fill: '#243746', ink: '#edf4f7'},
  ].map((background, index) => {
    const x = 60 + index * 368;
    return `<rect x="${x}" y="1060" width="344" height="192" rx="16" fill="${background.fill}"/>
      ${text(x + 20, 1093, background.name, 21, background.ink, 600)}
      ${['document', 'folder', 'world'].map((icon, i) => image(icon, 64, 'pending', x + 30 + i * 110, 1121) + text(x + 62 + i * 110, 1224, names[icon], 16, background.ink, 400, 'text-anchor="middle"')).join('')}`;
  }).join('');
  return `<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="1200" height="1520" viewBox="0 0 1200 1520" role="img" aria-labelledby="title description">
    <title id="title">Launcher: checking for installed versions at different icon sizes</title>
    <desc id="description">Normal and checking ${names[source].toLowerCase()} icons at 16, 24, 32, 40, 48, 64, 80, 96 and 128 DIP. Four slats through 32 DIP, five through 64, six above 64. Additional light, warm and dark background comparisons.</desc>
    <rect width="1200" height="1520" fill="#ffffff"/>
    <g font-family="Segoe UI, Arial, sans-serif">
      ${text(60, 52, 'DŁUGOSZ LAUNCHER  /  VERSION DISCOVERY', 16, '#526875', 650, 'letter-spacing="1.5"')}
      ${text(60, 103, 'Checking for installed versions', 39, '#153546', 650)}
      ${text(60, 140, 'The current icon stays usable while Launcher checks for available versions.', 21)}
      ${cards}
      ${text(60, 996, 'The same overlay on different backgrounds', 29, '#153546', 650)}
      ${text(60, 1030, '64 DIP examples with the current provisional artwork.', 20)}
      ${backgrounds}
      <rect x="60" y="1290" width="1080" height="148" rx="16" fill="#eef4f6"/>
      ${text(84, 1325, 'Point to an icon to see whether its version check is still in progress.', 21, '#183647', 600)}
      ${text(84, 1360, 'The static blinds clear when the check finishes or the pointer leaves.', 20)}
      ${text(84, 1395, 'Transparent gaps keep the icon visible. Existing version choices remain usable.', 20)}
      ${text(60, 1476, '4 slats: through 32 DIP   ·   5 slats: through 64 DIP   ·   6 slats: above 64 DIP', 18)}
      ${text(60, 1505, 'DIP = device-independent pixel. Native icon samples; layout is scaled for this illustration.', 16, '#627785')}
    </g>
  </svg>`;
}

const sheet = makeSheet(images);
const html = `<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Launcher - version-check overlay gallery</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#f5f7f8;color:#183647;font:16px/1.55 "Segoe UI",Arial,sans-serif}main{max-width:1200px;margin:auto;padding:40px 28px 60px}h1{font-size:36px;line-height:1.15;margin:12px 0 18px}h2{font-size:25px;margin:32px 0 12px}p{max-width:850px}.eyebrow{font-size:13px;letter-spacing:.12em;font-weight:650;color:#526875}.intro{font-size:19px}.controls{display:flex;flex-wrap:wrap;gap:20px;padding:20px;background:white;border:1px solid #d9e3e8;border-radius:12px;margin:24px 0}.controls label{display:grid;gap:5px;font-weight:600}select,button,.download{font:inherit;padding:8px 12px;border:1px solid #bacbd4;border-radius:6px;background:white;color:#183647}button,.download{cursor:pointer}button:hover,.download:hover{background:#e5eef2}button:focus-visible,select:focus-visible,a:focus-visible{outline:3px solid #2c779b;outline-offset:3px}.download{text-decoration:none;display:inline-block}.note{font-size:14px;color:#526875}.grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:18px}.card{background:white;border:1px solid #d9e3e8;border-radius:12px;overflow:hidden}.card header{padding:12px 16px;display:flex;justify-content:space-between;gap:8px;align-items:baseline}.card header strong{font-size:20px}.badge{font-size:14px;color:#526875}.pair{display:grid;grid-template-columns:1fr 1fr;gap:8px;padding:12px 16px;background:var(--panel,#f3c9b8)}figure{margin:0;display:grid;justify-items:center;align-content:center;gap:12px;min-height:174px}figure img{display:block}figcaption{font-size:14px;color:var(--ink,#183647)}.large{grid-column:1/-1}.large .pair{grid-template-columns:repeat(auto-fit,minmax(272px,1fr))}.large figure{min-height:318px}.extra{background:#e5eef2}.sheet{background:white;padding:12px;margin-top:18px;border:1px solid #d9e3e8}.sheet svg{display:block;width:100%;height:auto}.actions{display:flex;gap:12px;flex-wrap:wrap;align-items:center}.status{min-height:1.5em}footer{margin-top:36px;border-top:1px solid #d9e3e8;padding-top:20px}summary{cursor:pointer;font-size:22px;font-weight:650;margin:28px 0 12px}#boundaries{accent-color:#276281}@media(max-width:1020px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}@media(max-width:590px){main{padding:24px 14px}.grid{grid-template-columns:1fr}.large .pair{grid-template-columns:1fr}h1{font-size:30px}}@media print{body{background:white}main{padding:0}.controls,.interactive,.actions,.note,footer,.intro,.eyebrow,h1,details,body>main>h2{display:none}.sheet{border:0;padding:0;margin:0}.sheet svg{width:100%;max-height:255mm}h2.print-title{display:none}@page{size:A4 portrait;margin:12mm}}
</style>
<main><div class="eyebrow">DŁUGOSZ LAUNCHER / ARTWORK REFERENCE</div>
<h1>Checking for versions: the blinds overlay</h1>
<p class="intro">Point to an icon while its version check is queued or running: static blinds appear over that icon. The current icon and known version choices remain usable. The blinds clear when the check finishes or the pointer leaves.</p>
<div class="controls">
 <label>Icon <select id="source"><option value="folder">Folder</option><option value="document">Document</option><option value="world">World</option></select></label>
 <label>Panel <select id="theme"><option value="warm">Warm</option><option value="light">Light</option><option value="dark">Dark</option></select></label>
 <label>Native display scale <select id="scale"><option value="100">100%</option><option value="200" selected>200%</option></select></label>
 <label style="align-content:center"><span><input id="boundaries" type="checkbox"> Include 33 / 65 DIP boundaries</span></label>
</div>
<p class="note">DIP means device-independent pixel. Slat count follows the icon's drawn size, after Launcher fit scaling. A 200% display uses twice the pixels without adding slats. On-screen dimensions here also depend on your browser zoom and Windows display scaling. Both states are shown together for comparison; a real icon shows just one state at a time.</p>
<section class="interactive" aria-label="Icon size comparisons"><div id="grid" class="grid"></div></section>
<details><summary>Largest base icon size: 256 DIP</summary><div id="large" class="grid"></div></details>
<h2 class="print-title">Keep an illustration for the manual</h2>
<p class="note">This sheet follows your icon selection. SVG keeps the layout and text editable, with native PNG icon examples embedded. The PNG export is 2400 × 3040 pixels. All images are included in this file; it works offline.</p>
<div class="actions"><button id="png">Save manual PNG</button><button id="svg">Save manual SVG</button><button id="print">Print sheet</button><span id="status" class="status" role="status"></span></div>
<div id="sheet" class="sheet"></div>
<footer><strong>Current artwork:</strong> translucent dark slats, pale edges and open gaps. 4 slats through 32 DIP; 5 above 32 through 64; 6 above 64. No animation. Artwork remains provisional.<br><span class="note">Samples exported by Launcher's production IconImages, embedded SVG renderer and Direct2D compositor. Windows stock icon appearance can vary with Windows version.</span></footer>
</main>
<script>
const images=${JSON.stringify(images)};
const manifest=${JSON.stringify(manifest)};
const makeSheet=${makeSheet.toString()};
const names={folder:'Folder',document:'Document',world:'World'};
const themes={light:['#edf3f5','#183647'],warm:['#f3c9b8','#183647'],dark:['#243746','#edf4f7']};
const byId=id=>document.getElementById(id);
function settings(){return {source:byId('source').value,scale:Number(byId('scale').value),theme:byId('theme').value};}
function card(size){const {source,scale}=settings();const sample=manifest.samples.find(s=>s.size_dip===size&&s.scale_percent===scale);return '<article class="card'+(size===256?' large':'')+'"><header><strong>'+size+' DIP</strong><span class="badge">'+sample.slats+' slats · '+sample.pixels+' px</span></header><div class="pair">'+['ready','pending'].map(state=>'<figure><img alt="'+names[source]+', '+size+' DIP, '+(state==='ready'?'normal':'checking')+'" width="'+size+'" height="'+size+'" src="'+images[source+'-'+size+'-'+scale+'-'+state+'.png']+'"><figcaption>'+(state==='ready'?'Normal':'Checking')+'</figcaption></figure>').join('')+'</div></article>';}
function update(){const {source,scale,theme}=settings();const [panel,ink]=themes[theme];document.documentElement.style.setProperty('--panel',panel);document.documentElement.style.setProperty('--ink',ink);const sizes=byId('boundaries').checked?[16,24,32,33,40,48,64,65,80,96,128]:[16,24,32,40,48,64,80,96,128];byId('grid').innerHTML=sizes.map(card).join('');byId('large').innerHTML=card(256);byId('sheet').innerHTML=makeSheet(images,source,scale);byId('status').textContent='';}
function download(blob,name){const url=URL.createObjectURL(blob);const a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),2000);}
async function sheetPng(){const {source,scale}=settings();const svg=makeSheet(images,source,scale);const image=new Image();const url=URL.createObjectURL(new Blob([svg],{type:'image/svg+xml;charset=utf-8'}));try{await new Promise((resolve,reject)=>{image.onload=resolve;image.onerror=()=>reject(new Error('Could not render the manual sheet.'));image.src=url;});const canvas=document.createElement('canvas');canvas.width=2400;canvas.height=3040;canvas.getContext('2d').drawImage(image,0,0,2400,3040);return await new Promise((resolve,reject)=>canvas.toBlob(blob=>blob?resolve(blob):reject(new Error('PNG export failed.')),'image/png'));}finally{URL.revokeObjectURL(url);}}
byId('png').onclick=async()=>{try{byId('status').textContent='Preparing PNG…';download(await sheetPng(),'launcher-version-check-overlay.png');byId('status').textContent='PNG saved.';}catch(error){byId('status').textContent=error.message;}};
byId('svg').onclick=()=>{const {source,scale}=settings();download(new Blob([makeSheet(images,source,scale)],{type:'image/svg+xml;charset=utf-8'}),'launcher-version-check-overlay.svg');};
byId('print').onclick=()=>window.print();
for(const id of ['source','theme','scale','boundaries'])byId(id).onchange=update;
window.gallery={settings,update,makeSheet:()=>makeSheet(images,settings().source,settings().scale),sheetPng};update();
</script></html>`;

fs.mkdirSync(outputDirectory);
fs.writeFileSync(path.join(outputDirectory, 'gallery.html'), html);
fs.writeFileSync(path.join(outputDirectory, 'launcher-version-check-overlay.svg'), sheet);
fs.writeFileSync(path.join(outputDirectory, 'provenance.json'), JSON.stringify({
  renderer: manifest.renderer,
  icon_sources: manifest.icon_sources,
  sizes_dip: manifest.sizes_dip,
  scales_percent: manifest.scales_percent,
  embedded_native_images: Object.keys(images).length,
  source_svg_sha256: sourceHashes,
  native_png_sha256: hashes,
  notes: 'The self-contained gallery embeds native ready/pending composites. Manual SVG labels/layout are editable; icon examples remain raster images. Artwork is provisional.',
}, null, 2) + '\n');
console.log(path.resolve(outputDirectory, 'gallery.html'));
