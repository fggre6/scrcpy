// Tauri v2 exposes its JS API on window.__TAURI__ only when
// app.withGlobalTauri = true. Fail loudly here instead of leaving every
// button silently dead (the bug this replaced).
if (!window.__TAURI__ || !window.__TAURI__.core || !window.__TAURI__.core.invoke) {
  document.addEventListener('DOMContentLoaded', () => {
    const t = document.getElementById('toast');
    if (t) {
      t.textContent = 'Tauri API 未加载：请确认 tauri.conf.json 里 app.withGlobalTauri = true';
      t.classList.add('show');
    }
  });
  throw new Error('window.__TAURI__.core.invoke unavailable; check withGlobalTauri');
}
const { invoke } = window.__TAURI__.core;

const $ = (id) => document.getElementById(id);
let options = null;
let mappingConfig = null;
let templates = [];
let currentTemplate = null;
let varsDraft = [];

const ARG_GROUPS = {
  '连接': [['serial','文本','设备序列号'],['tcpip','开关','通过 TCP/IP 连接']],
  '显示': [['window-size','文本','窗口尺寸 宽x高'],['fullscreen','开关','全屏'],['always-on-top','开关','窗口置顶'],['rotation','数字','旋转角度'],['no-window','开关','不显示窗口']],
  '视频': [['max-size','文本','最大尺寸'],['bit-rate','文本','码率 如 8M'],['fps','数字','帧率'],['video-codec','文本','视频编码'],['crop','文本','裁剪 宽x高+x+y']],
  '音频': [['no-audio','开关','禁用音频'],['audio-source','文本','音频来源']],
  '输入': [['keyboard','文本','键盘模式'],['mouse','文本','鼠标模式'],['gamepad','文本','手柄模式'],['no-control','开关','禁止控制']],
  '设备': [['turn-screen-off','开关','关闭设备屏幕'],['stay-awake','开关','保持唤醒'],['show-touches','开关','显示触摸点']]
};
const TYPES = ['文本','数字','开关','下拉'];

function toast(msg){ const t=$('toast'); t.textContent=msg; t.classList.add('show'); clearTimeout(toast._t); toast._t=setTimeout(()=>t.classList.remove('show'),2600); }
function err(e){ toast('错误: '+(e?.message||e)); console.error(e); }

function applyTheme(name){
  document.body.dataset.theme = name;
  if(options){ options.theme = name; }
  $('themeSelect') && ($('themeSelect').value = name);
}

async function loadAll(){
  try{
    options = await invoke('read_launch_options');
    mappingConfig = await invoke('read_mapping_config');
    templates = await invoke('list_templates');
    const info = await invoke('get_app_info');
    $('sideVersion').textContent = 'v'+info.version;
    applyTheme(options.theme || 'hive-light');
    $('deviceInput').value = options.device || '';
    $('extraArgs').value = options.extra_args || '';
    varsDraft = Object.entries(options.variables||{}).map(([k,v])=>({k,v}));
    renderVars(); renderArgs(); renderTemplates(); renderSummary(); renderMappingCfg();
    $('appInfo').innerHTML = dl({
      '版本': info.version, '安装目录': info.install_dir,
      '模板数量': info.template_count, '核心程序': info.core_exists?'存在':'缺失 (dll\\scrcpy-core.exe)'
    });
    $('heroStatus').textContent = info.core_exists ? '核心程序就绪，点击启动即可投屏' : '警告：dll\\scrcpy-core.exe 不存在';
    await refreshPreview();
  }catch(e){ err(e); }
}

function dl(obj){
  return '<dl>'+Object.entries(obj).map(([k,v])=>`<dt>${esc(k)}</dt><dd>${esc(String(v))}</dd>`).join('')+'</dl>';
}
function esc(s){ return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }

function renderArgs(){
  const host=$('argsForm'); host.innerHTML='';
  for(const [group, items] of Object.entries(ARG_GROUPS)){
    const box=document.createElement('div');
    box.innerHTML=`<h3 style="font-size:12px;color:var(--muted);margin:6px 0 2px;grid-column:1/-1">${group}</h3>`;
    host.appendChild(box);
    for(const [key,type,label] of items){
      const wrap=document.createElement('div'); wrap.className='arg-item';
      const val=(options.arguments||{})[key];
      if(type==='开关'){
        wrap.innerHTML=`<label><input type="checkbox" data-arg="${key}" ${val===true?'checked':''}> ${label}</label>`;
      } else {
        wrap.innerHTML=`<label>${label}</label><input type="text" data-arg="${key}" value="${esc(val??'')}" placeholder="--${key}">`;
      }
      host.appendChild(wrap);
    }
  }
  host.querySelectorAll('[data-arg]').forEach(el=>{
    el.addEventListener('change',()=>syncArgs()); el.addEventListener('input',()=>syncArgs());
  });
}
function syncArgs(){
  document.querySelectorAll('[data-arg]').forEach(el=>{
    const key=el.dataset.arg;
    options.arguments[key] = el.type==='checkbox' ? el.checked : el.value;
  });
  options.extra_args = $('extraArgs').value;
  options.device = $('deviceInput').value;
  refreshPreview();
}
async function refreshPreview(){
  try{
    const argv = await invoke('build_launch_command',{options});
    $('commandPreview').textContent = 'scrcpy-core.exe '+argv.map(a=>/\s/.test(a)?`"${a}"`:a).join(' ');
    renderSummary();
  }catch(e){ $('commandPreview').textContent = '错误: '+(e?.message||e); }
}
function renderSummary(){
  const a=options.arguments||{};
  $('configSummary').innerHTML=dl({
    '设备': options.device||'(自动)',
    '分辨率': a['max-size']||'(默认)',
    '码率': a['bit-rate']||'(默认)',
    '帧率': a['fps']||'(默认)',
    '活动模板': options.active_template||'default',
    '映射开关': mappingConfig?.enabled?'开启':'关闭'
  });
}

function renderVars(){
  const host=$('varRows'); host.innerHTML='';
  varsDraft.forEach((v,i)=>{
    const row=document.createElement('div'); row.className='var-row';
    row.innerHTML=`<input placeholder="变量名" value="${esc(v.k)}" data-vk="${i}"><input placeholder="值" value="${esc(v.v)}" data-vv="${i}"><button class="btn ghost small" data-vd="${i}">删除</button>`;
    host.appendChild(row);
  });
  host.querySelectorAll('[data-vk]').forEach(el=>el.oninput=()=>{varsDraft[+el.dataset.vk].k=el.value;});
  host.querySelectorAll('[data-vv]').forEach(el=>el.oninput=()=>{varsDraft[+el.dataset.vv].v=el.value;});
  host.querySelectorAll('[data-vd]').forEach(el=>el.onclick=()=>{varsDraft.splice(+el.dataset.vd,1);renderVars();});
}

function renderTemplates(){
  const host=$('tplList'); host.innerHTML='';
  if(!templates.length){ host.innerHTML='<p class="hint">templates\\ 下没有 JSON 模板</p>'; }
  for(const t of templates){
    const item=document.createElement('div');
    const active=t.file.replace(/\.json$/i,'')===options.active_template;
    item.className='tpl-item'+(currentTemplate===t.file?' active':'');
    item.innerHTML=`<div class="t">${esc(t.name||t.file)} ${active?'<span class="badge">活动</span>':''}</div><div class="s">${esc(t.file)} · ${t.count} 条映射</div>`;
    item.onclick=async()=>{
      try{ currentTemplate=t.file; const c=await invoke('read_template',{file:t.file}); $('tplEditor').value=JSON.stringify(c,null,2); $('tplStatus').textContent=t.file; renderTemplates(); }catch(e){err(e);}
    };
    host.appendChild(item);
  }
}
function renderMappingCfg(){
  const c=mappingConfig||{};
  $('mappingCfg').innerHTML=dl({
    '启用映射': c.enabled?'是':'否', '活动模板': c.active_template||'default',
    '切换模板': c.shortcuts?.switch_template||'F2', '设计模式': c.shortcuts?.design_mode||'F3',
    '映射开关': c.shortcuts?.toggle_mapping||'F4', '管理面板': c.shortcuts?.manager_panel||'F5'
  });
}

async function saveOptions(){
  try{
    options.variables = Object.fromEntries(varsDraft.filter(v=>v.k).map(v=>[v.k,v.v]));
    await invoke('write_launch_options',{options});
    await refreshPreview();
  }catch(e){err(e);}
}

// ---- navigation ----
document.querySelectorAll('.nav li').forEach(li=>{
  li.onclick=()=>{
    document.querySelectorAll('.nav li').forEach(x=>x.classList.remove('active'));
    li.classList.add('active');
    const page=li.dataset.page;
    document.querySelectorAll('.page').forEach(p=>p.classList.add('hidden'));
    $('page-'+page).classList.remove('hidden');
    $('pageTitle').textContent=li.textContent.trim();
  };
});

$('themeToggle').onclick=()=>{ applyTheme(document.body.dataset.theme==='hive-dark'?'hive-light':'hive-dark'); saveOptions(); };
$('themeSelect').onchange=(e)=>{ applyTheme(e.target.value); saveOptions(); };

// ---- launch page ----
$('deviceInput').onchange=()=>syncArgs();
$('extraArgs').oninput=()=>syncArgs();
$('btnRefreshDevices').onclick=async()=>{
  try{ const ds=await invoke('list_devices'); $('deviceList').innerHTML = ds.length?ds.map(d=>`<span class="chip" data-dev="${esc(d)}">${esc(d)}</span>`).join(''):'<span class="hint">未发现设备</span>';
    $('deviceList').querySelectorAll('.chip').forEach(c=>c.onclick=()=>{$('deviceInput').value=c.dataset.dev;syncArgs();});
  }catch(e){err(e);}
};
$('btnLaunch').onclick=async()=>{
  try{ await saveOptions(); const r=await invoke('launch_scrcpy',{options}); toast('已启动: '+r); }
  catch(e){err(e);}
};
$('btnStop').onclick=async()=>{ try{ await invoke('stop_scrcpy'); toast('已停止'); }catch(e){err(e);} };
$('btnCopyCmd').onclick=()=>{ navigator.clipboard.writeText($('commandPreview').textContent).then(()=>toast('已复制')).catch(err); };

// ---- variables ----
$('btnAddVar').onclick=()=>{ varsDraft.push({k:'',v:''}); renderVars(); };

// ---- templates ----
$('btnSaveTpl').onclick=async()=>{
  if(!currentTemplate){ toast('请先选择一个模板'); return; }
  try{ const v=JSON.parse($('tplEditor').value); await invoke('save_template',{file:currentTemplate,content:v}); toast('模板已保存'); templates=await invoke('list_templates'); renderTemplates(); }
  catch(e){err(e);}
};
$('btnNewTpl').onclick=async()=>{
  const name=prompt('新模板文件名（不含 .json）'); if(!name) return;
  const file=name.endsWith('.json')?name:name+'.json';
  try{ await invoke('save_template',{file,content:{name,mappings:[]}}); templates=await invoke('list_templates'); currentTemplate=file; $('tplEditor').value=JSON.stringify({name,mappings:[]},null,2); $('tplStatus').textContent=file; renderTemplates(); }
  catch(e){err(e);}
};
$('btnDupTpl').onclick=async()=>{
  if(!currentTemplate){toast('请先选择模板');return;}
  const name=prompt('新模板文件名（不含 .json）'); if(!name) return;
  try{ await invoke('duplicate_template',{file:currentTemplate,newFile:name.endsWith('.json')?name:name+'.json'}); templates=await invoke('list_templates'); renderTemplates(); }
  catch(e){err(e);}
};
$('btnDelTpl').onclick=async()=>{
  if(!currentTemplate){toast('请先选择模板');return;}
  if(!confirm('确定删除 '+currentTemplate+'？')) return;
  try{ await invoke('delete_template',{file:currentTemplate}); currentTemplate=null; $('tplEditor').value=''; $('tplStatus').textContent='未选择'; templates=await invoke('list_templates'); renderTemplates(); }
  catch(e){err(e);}
};
$('btnSaveMapping').onclick=async()=>{
  const enabled=confirm('是否启用映射？点“确定”=启用，点“取消”=禁用');
  mappingConfig.enabled=enabled;
  try{ await invoke('write_mapping_config',{config:mappingConfig}); renderMappingCfg(); toast('映射配置已保存'); }catch(e){err(e);}
};

// ---- settings ----
$('btnOpenDir').onclick=async()=>{ try{ await invoke('open_install_dir'); }catch(e){err(e);} };

loadAll();
