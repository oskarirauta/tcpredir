'use strict';
'require baseclass';
'require poll';
'require rpc';
'require dom';

/* A compact "what does this router forward" card for Status -> Overview.

   tcpredir and uxcd are separate programs that cooperate but do not depend on
   each other, and this widget keeps that honest: the tcpredir redirects are
   always shown, the container ports are asked for only best-effort. If uxcd is
   not installed the ubus call simply resolves to nothing and its rows are
   skipped - the card still renders. The full, editable view lives on the
   tcpredir Status tab; this is only the glance. */

var callList = rpc.declare({
	object: 'tcpredir',
	method: 'list'
});

var callUxcdList = rpc.declare({
	object: 'uxcd',
	method: 'list'
});

function hostport(ip, port) {
	var a = (ip == null || ip === '' || ip === '0.0.0.0') ? '*' : ip;
	return a + ':' + port;
}

/* "[ip:]hostport:containerport[/proto]" - the form uxcd stores in `ports`. */
function parsePublished(spec) {
	var s    = String(spec);
	var m    = s.match(/\/(\w+)$/);
	var prot = m ? m[1] : 'tcp';
	var f    = s.replace(/\/\w+$/, '').split(':');

	if (f.length === 2) return { bind: null, host: f[0], target: f[1], proto: prot };
	if (f.length === 3) return { bind: f[0], host: f[1], target: f[2], proto: prot };
	return null;
}

function tag(text, color, title) {
	return E('span', {
		'style': 'display:inline-block;padding:0 .5em;border-radius:.8em;font-size:85%;' +
			'white-space:nowrap;background:' + color + ';color:#fff',
		'title': title || ''
	}, text);
}

/* One flat list of rows, each carrying enough to sort and label itself, so the
   tcpredir redirects and the container ports can be merged for display without
   the reader losing track of which daemon owns which line. */
function collect(tcp, uxc) {
	var rows = [];

	((tcp && tcp.redirects) || []).forEach(function(r) {
		rows.push({
			sort:   r.listen_port || 0,
			proto:  String(r.proto || 'tcp').toUpperCase(),
			listen: hostport(r.listen_ip, r.listen_port),
			target: hostport(r.target_ip, r.target_port),
			via:    (r.source === 'config')
				? tag(_('config'), '#5b7', _('Defined in /etc/config/tcpredir'))
				: tag(_('runtime'), '#78a', _('Added at runtime; gone on restart'))
		});
	});

	Object.keys(uxc || {}).sort().forEach(function(name) {
		var c = uxc[name];
		(c.ports || []).forEach(function(spec) {
			var p = parsePublished(spec);
			if (!p) return;

			var color = c.ports_published ? '#c90' : (c.ports_error ? '#c44' : '#999');
			var ttl   = c.ports_error ? c.ports_error
				: (c.ports_published ? _('Forwarded by uxcd for this container')
				                     : _('Container port not currently listening'));

			rows.push({
				sort:   parseInt(p.host, 10) || 0,
				proto:  String(p.proto || 'tcp').toUpperCase(),
				listen: hostport(p.bind, p.host),
				target: name + ':' + p.target,
				via:    tag(name, color, ttl)
			});
		});
	});

	rows.sort(function(a, b) { return a.sort - b.sort; });
	return rows;
}

return baseclass.extend({
	title: _('Port forwards'),

	load: function() {
		return Promise.all([
			L.resolveDefault(callList(), null),
			L.resolveDefault(callUxcdList(), null)
		]);
	},

	rows: function(data) {
		var list = collect(data[0], data[1]);

		if (!list.length)
			return [ E('div', { 'class': 'tr' },
				E('div', { 'class': 'td' }, E('em', _('Nothing is being forwarded.')))) ];

		return list.map(function(r) {
			return E('div', { 'class': 'tr' }, [
				E('div', { 'class': 'td', 'style': 'width:3.5em' },
					E('span', { 'class': 'ifacebadge' }, r.proto)),
				E('div', { 'class': 'td', 'style': 'white-space:nowrap' }, r.listen),
				E('div', { 'class': 'td', 'style': 'white-space:nowrap;color:#888' },
					'\u2192 ' + r.target),
				E('div', { 'class': 'td', 'style': 'width:100%' }, ''),
				E('div', { 'class': 'td', 'style': 'text-align:right;white-space:nowrap' }, r.via)
			]);
		});
	},

	refresh: function() {
		var self = this;
		return this.load().then(function(data) {
			var el = document.getElementById('tcpredir-widget');
			if (el)
				dom.content(el, self.rows(data));
		});
	},

	render: function(data) {
		var self  = this;
		var table = E('div', { 'class': 'table', 'id': 'tcpredir-widget' }, this.rows(data));
		poll.add(function() { return self.refresh(); }, 5);
		return table;
	}
});
