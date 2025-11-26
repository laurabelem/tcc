async function obterEstado() {
  try {
    const r = await fetch('/api/state');
    const d = await r.json();

    document.getElementById('tempAtual').textContent =
      isNaN(d.temp) ? '-- °C' : d.temp.toFixed(1) + ' °C';

    document.getElementById('umidAtual').textContent =
      isNaN(d.umid) ? '-- %' : d.umid.toFixed(1) + ' %';

    document.getElementById('setpointAtual').textContent =
      d.alvo.toFixed(1) + ' °C';

    document.getElementById('valorSetpoint').value =
      d.alvo.toFixed(1);

    document.getElementById('estadoLampada').textContent =
      d.lampada ? 'Ligada' : 'Desligada';
  } catch (e) {
    console.error(e);
  }
}

async function obterHoje() {
  try {
    const r = await fetch('/api/today');
    const lista = await r.json();
    atualizarTabela(lista);
    atualizarGrafico(lista);
  } catch (e) {
    console.error(e);
  }
}

function atualizarTabela(lista) {
  const tbody = document.querySelector('#tabela tbody');
  tbody.innerHTML = '';

  lista.forEach(p => {
    const dataStr = p.data === '0000-00-00' ? '-' : p.data;
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${dataStr}</td>
      <td>${p.hora}</td>
      <td>${p.temp.toFixed(1)}</td>
      <td>${p.umid.toFixed(1)}</td>`;
    tbody.appendChild(tr);
  });
}

function atualizarGrafico(lista) {
  const tLine = document.getElementById('linhaTemp');
  const uLine = document.getElementById('linhaUmid');
  const gTemp = document.getElementById('pontosTemp');
  const gUmid = document.getElementById('pontosUmid');

  if (!lista.length) {
    tLine.setAttribute('points', '');
    uLine.setAttribute('points', '');
    gTemp.innerHTML = '';
    gUmid.innerHTML = '';
    return;
  }

  let minT = Infinity, maxT = -Infinity;
  let minU = Infinity, maxU = -Infinity;

  lista.forEach(p => {
    minT = Math.min(minT, p.temp);
    maxT = Math.max(maxT, p.temp);
    minU = Math.min(minU, p.umid);
    maxU = Math.max(maxU, p.umid);
  });

  if (maxT - minT < 1) { maxT += 0.5; minT -= 0.5; }
  if (maxU - minU < 1) { maxU += 0.5; minU -= 0.5; }

  const n = lista.length;
  const ptsT = [];
  const ptsU = [];
  let htmlTemp = "";
  let htmlUmid = "";

  lista.forEach((p, idx) => {
    const x = 7 + (n === 1 ? 0 : (idx / (n - 1)) * 90);

    const yT = 65 - ((p.temp - minT) / (maxT - minT)) * 60;
    const yU = 65 - ((p.umid - minU) / (maxU - minU)) * 60;

    ptsT.push(`${x},${yT.toFixed(2)}`);
    ptsU.push(`${x},${yU.toFixed(2)}`);

    htmlTemp += `<circle cx="${x}" cy="${yT.toFixed(2)}" r="0.9" class="point-temp"></circle>`;
    htmlUmid += `<circle cx="${x}" cy="${yU.toFixed(2)}" r="0.9" class="point-umid"></circle>`;
  });

  tLine.setAttribute('points', ptsT.join(' '));
  uLine.setAttribute('points', ptsU.join(' '));

  gTemp.innerHTML = htmlTemp;
  gUmid.innerHTML = htmlUmid;
}

async function enviarSetpoint(e) {
  e.preventDefault();
  const valor = document.getElementById('valorSetpoint').value;

  const fd = new FormData();
  fd.append('valor', valor);

  try {
    await fetch('/api/setpoint', {
      method: 'POST',
      body: fd
    });
    await obterEstado();
    alert('Setpoint atualizado.');
  } catch (err) {
    console.error(err);
    alert('Falha ao atualizar setpoint.');
  }
}

async function obterNetStatus() {
  try {
    const r = await fetch('/api/netstatus');
    const d = await r.json();

    const status = d.sta_connected
      ? 'Conectado'
      : (d.sta_configured ? 'Tentando conectar' : 'Desconectado');

    document.getElementById('netStatus').textContent = status;

    if (d.time_valid && d.datetime) {
      document.getElementById('netTime').textContent = d.datetime;
    } else {
      document.getElementById('netTime').textContent = 'Offline';
    }

    document.getElementById('netSsid').textContent =
      d.ssid && d.ssid.length ? d.ssid : 'Não configurado';

    if (d.ssid && d.ssid.length) {
      document.getElementById('wifiSsid').value = d.ssid;
    }
  } catch (e) {
    console.error(e);
  }
}

async function enviarWifiConfig(e) {
  e.preventDefault();
  const ssid = document.getElementById('wifiSsid').value;
  const pass = document.getElementById('wifiPass').value;

  const fd = new FormData();
  fd.append('ssid', ssid);
  fd.append('senha', pass);

  try {
    await fetch('/api/wifi', {
      method: 'POST',
      body: fd
    });
    await obterNetStatus();
    alert('Configuração de Wi-Fi enviada. O ESP32 tentará conectar.');
  } catch (err) {
    console.error(err);
    alert('Falha ao enviar configuração de Wi-Fi.');
  }
}

async function sincronizarNtpAgora() {
  try {
    await fetch('/api/ntpsync');
    await obterNetStatus();
    alert('Solicitação de sincronização enviada.');
  } catch (err) {
    console.error(err);
    alert('Falha ao solicitar sincronização.');
  }
}

document.getElementById('formSetpoint')
  .addEventListener('submit', enviarSetpoint);

document.getElementById('wifiForm')
  .addEventListener('submit', enviarWifiConfig);

document.getElementById('syncNtpBtn')
  .addEventListener('click', sincronizarNtpAgora);

function atualizarTudo() {
  obterEstado();
  obterHoje();
  obterNetStatus();
}

atualizarTudo();
setInterval(atualizarTudo, 60000);
