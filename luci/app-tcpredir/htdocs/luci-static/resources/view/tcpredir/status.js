'use strict';
'require view';
'require poll';
'require ui';
'require rpc';
'require dom';

/* Everything this router forwards, in one place.

   Two daemons answer for it and neither owns the other's entries: tcpredir
   reports the redirects it serves, and uxcd reports the ports its containers
   publish. They are listed separately and labelled, because the thing a reader
   needs to know first is where to go to change one. Guessing that from a flat
   merged list is exactly the mistake this layout avoids. */

var callList = rpc.declare({
	object: 'tcpredir',
	method: 'list'
});

var callAdd = rpc.declare({
	object: 'tcpredir',
	method: 'add',
	params: [ 'redirect' ]
});

var callRemove = rpc.declare({
	object: 'tcpredir',
	method: 'remove',
	params: [ 'name' ]
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

/* What the forwarder is doing, which is not the same question as what the
   container is doing: a container runs happily while its published ports are
   dead - no address in its netns yet, tcpredir missing, the child out of
   restarts. Reporting the container's state here would promise a port that
   nothing is listening on. */
function publishedState(c) {
	if (c.ports_published)
		return E('span', { 'style': 'color:#4caf50' }, _('listening'));

	if (c.ports_error)
		return E('span', { 'style': 'color:#f44336', 'title': c.ports_error }, _('failed'));

	if (!c.running)
		return E('span', { 'style': 'color:#888' }, _('container stopped'));

	return E('span', { 'style': 'color:#f44336' }, _('not published'));
}

return view.extend({
	handleAdd: function() {
		var self = this;

		ui.showModal(_('Add redirect'), [
			E('p', _('Starts a redirect immediately. It is not written to the configuration file, so it disappears when the daemon restarts - and a reload leaves it alone.')),
			E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' }, _('Redirect')),
				E('div', { 'class': 'cbi-value-field' }, [
					E('input', {
						'type': 'text',
						'class': 'cbi-input-text',
						'id': 'tcpredir-add-spec',
						'style': 'width:100%',
						'placeholder': '[listen_ip:]listen_port:target_ip:target_port[/proto]'
					}),
					E('div', { 'class': 'cbi-value-description' },
						_('For example %s, or %s to accept only on the LAN address.')
							.format('<code>8080:10.0.0.99:80</code>', '<code>192.168.1.1:8080:10.0.0.99:80</code>'))
				])
			]),
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
				' ',
				E('button', {
					'class': 'btn cbi-button-action important',
					'click': ui.createHandlerFn(this, function() {
						var spec = document.getElementById('tcpredir-add-spec').value.trim();
						if (!spec)
							return;

						return callAdd(spec).then(function(res) {
							if (res && res.error) {
								ui.addNotification(null, E('p', res.error), 'error');
								return;
							}
							ui.hideModal();
							ui.addNotification(null,
								E('p', _('Added %s.').format(res && res.name ? res.name : spec)), 'info');
							return self.refresh();
						});
					})
				}, _('Add'))
			])
		]);
	},

	handleRemove: function(name) {
		var self = this;
		return callRemove(name).then(function(res) {
			if (res && res.error) {
				ui.addNotification(null, E('p', res.error), 'error');
				return;
			}
			return self.refresh();
		});
	},

	renderRedirects: function(data) {
		var self  = this,
		    list  = (data && data.redirects) || [],
		    rows  = [];

		list.sort(function(a, b) { return (a.listen_port || 0) - (b.listen_port || 0); });

		rows.push(E('div', { 'class': 'tr table-titles' }, [
			E('div', { 'class': 'th', 'style': 'width:10%' }, _('Protocol')),
			E('div', { 'class': 'th' }, _('Listening on')),
			E('div', { 'class': 'th' }, _('Forwards to')),
			E('div', { 'class': 'th', 'style': 'text-align:center;width:15%' }, _('Connections')),
			E('div', { 'class': 'th', 'style': 'text-align:center;width:18%' }, _('Source')),
			E('div', { 'class': 'th', 'style': 'text-align:right;width:15%' }, _('Action'))
		]));

		if (!list.length)
			rows.push(E('div', { 'class': 'tr placeholder' },
				E('div', { 'class': 'td' }, E('em', _('No redirects are running.')))));

		list.forEach(function(r) {
			var fromConfig = (r.source === 'config');

			rows.push(E('div', { 'class': 'tr' }, [
				E('div', { 'class': 'td', 'data-title': _('Protocol') },
					E('span', { 'class': 'ifacebadge' }, String(r.proto || 'tcp').toUpperCase())),
				E('div', { 'class': 'td', 'data-title': _('Listening on') },
					hostport(r.listen_ip, r.listen_port)),
				E('div', { 'class': 'td', 'data-title': _('Forwards to') },
					hostport(r.target_ip, r.target_port)),
				E('div', { 'class': 'td', 'data-title': _('Connections'), 'style': 'text-align:center' },
					String(r.connections != null ? r.connections : '-') +
						(r.max_connections ? ' / ' + r.max_connections : '')),
				E('div', { 'class': 'td', 'data-title': _('Source'), 'style': 'text-align:center' },
					fromConfig
						? E('span', { 'title': _('Defined in /etc/config/tcpredir. Edit it on the Redirects page.') },
							_('Configuration file'))
						: E('span', { 'title': _('Added at runtime. It will not survive a restart of the daemon.') },
							_('Runtime'))),
				E('div', { 'class': 'td', 'style': 'text-align:right;width:15%;padding:5px 0 5px 5px' },
					fromConfig
						? E('em', { 'style': 'color:#888' }, _('in configuration'))
						: E('button', {
							'class': 'btn cbi-button cbi-button-negative',
							'click': ui.createHandlerFn(self, 'handleRemove', r.name)
						}, _('Remove')))
			]));
		});

		return E('div', { 'class': 'table' }, rows);
	},

	/* uxcd's published ports. Read-only on purpose: the container's configuration
	   decides these, and a redirect removed here would come straight back the next
	   time the container started. */
	renderPublished: function(containers) {
		var rows = [];

		Object.keys(containers || {}).sort().forEach(function(name) {
			var c = containers[name];

			(c.ports || []).forEach(function(spec) {
				var p = parsePublished(spec);
				if (!p)
					return;

				rows.push(E('div', { 'class': 'tr' }, [
					E('div', { 'class': 'td', 'data-title': _('Protocol') },
						E('span', { 'class': 'ifacebadge' }, p.proto.toUpperCase())),
					E('div', { 'class': 'td', 'data-title': _('Listening on') },
						hostport(p.bind, p.host)),
					E('div', { 'class': 'td', 'data-title': _('Container') }, [
						E('strong', {}, name),
						document.createTextNode(' : ' + p.target)
					]),
					E('div', { 'class': 'td', 'data-title': _('State'), 'style': 'text-align:center' },
						publishedState(c))
				]));
			});
		});

		if (!rows.length)
			return null;

		rows.unshift(E('div', { 'class': 'tr table-titles' }, [
			E('div', { 'class': 'th', 'style': 'width:10%' }, _('Protocol')),
			E('div', { 'class': 'th' }, _('Listening on')),
			E('div', { 'class': 'th' }, _('Container')),
			E('div', { 'class': 'th', 'style': 'text-align:center;width:18%' }, _('State'))
		]));

		return E('div', {}, [
			E('h3', _('Published container ports')),
			E('p', {}, [
				document.createTextNode(_('These are forwarded by uxcd for as long as the container runs, each by its own tcpredir process. They are not part of this daemon\'s configuration - change them where the container is defined, under ')),
				E('a', { 'href': L.url('admin', 'containers', 'overview') }, _('Containers')),
				document.createTextNode('.')
			]),
			E('div', { 'class': 'table' }, rows)
		]);
	},

	refresh: function() {
		var self = this;

		return Promise.all([
			callList(),
			/* uxcd is optional - if it is not installed the section is simply absent. */
			callUxcdList().catch(function() { return null; })
		]).then(function(res) {
			var data = res[0] || {}, cont = res[1];

			var header = E('p', {}, [
				document.createTextNode(_('tcpredir %s, configuration %s.')
					.format(data.version || '?', data.config || _('none'))),
				document.createTextNode(' '),
				E('em', _('Redirects reload without dropping established connections.'))
			]);

			var parts = [
				header,
				E('div', { 'style': 'margin:.5em 0' },
					E('button', {
						'class': 'btn cbi-button-add',
						'click': ui.createHandlerFn(self, 'handleAdd')
					}, _('Add redirect'))),
				self.renderRedirects(data)
			];

			var pub = cont ? self.renderPublished(cont) : null;
			if (pub)
				parts.push(pub);

			var view = document.getElementById('tcpredir-status');
			if (view)
				dom.content(view, parts);

			return parts;
		});
	},

	render: function() {
		var self = this;

		poll.add(function() { return self.refresh(); }, 5);

		return this.refresh().then(function(parts) {
			return E('div', { 'class': 'cbi-map' }, [
				E('h2', _('Port Redirects')),
				E('div', { 'id': 'tcpredir-status' }, parts)
			]);
		});
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
