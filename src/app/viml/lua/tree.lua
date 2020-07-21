-- vim: set sw=2 sts=4 et tw=78 foldmethod=indent:
-- :luafile %
local api = vim.api
local inspect = vim.inspect
local fn = vim.fn
local eval = vim.api.nvim_eval
local C = vim.api.nvim_command
local cmd = vim.api.nvim_command
local buf_is_loaded = vim.api.nvim_buf_is_loaded
local call = vim.api.nvim_call_function

local is_windows = fn.has('win32') == 1 or fn.has('win64') == 1
local is_macos = not is_windows and fn.has('win32unix') == 0 and fn.has('macunix') == 1
local is_linux = fn.has('unix') == 1 and fn.has('macunix') == 0 and fn.has('win32unix') == 0
local info = debug.getinfo(1, "S")
local sfile = info.source:sub(2) -- remove @
local project_root = fn.fnamemodify(sfile, ':h:h')

local M = {}

--- Resume tree window.
-- If the window corresponding to bufnrs is available, goto it;
-- otherwise, create a new window.
-- @param bufnrs table: trees bufnrs ordered by recently used.
-- @return nil.
function M.resume(bufnrs, cfg)
  if bufnrs == nil then
    return
  end
  if type(bufnrs) == 'number' then
    bufnrs = {bufnrs}
  end

  -- check bufnrs
  local deadbufs = {}
  local treebufs = {}
  for i, bufnr in pairs(bufnrs) do
    loaded = buf_is_loaded(bufnr)
    if loaded then
      table.insert(treebufs, bufnr)
    else
      table.insert(deadbufs, bufnr)
    end
  end
  -- print("treebufs:", vim.inspect(treebufs))

  local find = false
  -- TODO: send delete notify when -1.
  for i, bufnr in pairs(treebufs) do
    local winid = call('bufwinid', {bufnr})
    if winid > 0 then
      print('goto winid', winid)
      call('win_gotoid', {winid})
      find = true
      break
    end
  end

  local bufnr = treebufs[1]
  local resize_cmd, str
  -- local no_split = false
  -- if cfg.split == 'no' or cfg.split == 'tab' or cfg.split == 'floating' then
  --     no_split = true
  -- end
  local vertical = ''
  local command = 'sbuffer'
  if cfg.split == 'tab' then
    cmd('tabnew')
  end
  if cfg.split == 'vertical' then
    vertical = 'vertical'
    resize_cmd = string.format('vertical resize %d', cfg['winwidth'])
  elseif cfg.split == 'horizontal' then
    resize_cmd = string.format('resize %d', cfg.winheight)
  elseif cfg.split == 'floating' then
    local winid = api.nvim_open_win(bufnr, true, {
      relative='editor',
      row=cfg.winrow,
      col=cfg.wincol,
      width=cfg.winwidth,
      height=cfg.winheight,
    })
  else
    command = 'buffer'
  end

  if cfg.split ~= 'floating' then
    local direction = 'topleft'
    if cfg.direction == 'botright' then
      direction = 'botright'
    end
    str = string.format("silent keepalt %s %s %s %d", direction, vertical, command, bufnr)

    if not find then
      cmd(str)
    end

    cmd(resize_cmd)
  end

  cmd("se nonu");
  cmd("se nornu");
  cmd("se nolist");
  cmd("se signcolumn=no");
  api.nvim_win_set_option(winid, 'wrap', false)
end

--- Drop file.
-- If the window corresponding to file is available, goto it;
-- otherwise, goto prev window and edit file.
-- @param file  string: file absolute path.
-- @return nil.
function M.drop(args, file)
  local arg = args[1] or 'edit'
  local bufnr = call('bufnr', {file})
  local winids = call('win_findbuf', {bufnr})
  -- print(vim.inspect(winids))
  if #winids == 1 then
    call('win_gotoid', {winids[1]})
  else
    local prev_winnr = call('winnr', {'#'})
    local prev_winid = call('win_getid', {prev_winnr})
    call('win_gotoid', {prev_winid})
    local str = string.format("%s %s", arg, file)
    cmd(str)
  end
end

-- 仅仅用于处理同名文件
-- def check_overwrite(view: View, dest: Path, src: Path) -> Path:
-- dest/src: {mtime=, path=, size=}
function M.pre_paste(pos, dest, src)
  -- print(vim.inspect(dest))
  local d_mtime = dest.mtime
  local s_mtime = src.mtime

  local slocaltime = os.date("%Y-%m-%d %H:%M:%S", s_mtime)
  local dlocaltime = os.date("%Y-%m-%d %H:%M:%S", d_mtime)
  -- time.strftime("%c", time.localtime(s_mtime))
  local msg1 = string.format(' src: %s %d bytes\n', src.path, src.size)
  local msg2 = string.format('      %s\n', slocaltime)
  local msg3 = string.format('dest: %s %d bytes\n', dest.path, dest.size)
  local msg4 = string.format('      %s\n', dlocaltime)
  local msg = msg1..msg2..msg3..msg4
  -- print_message(msg)

  local msg = msg..string.format('%s already exists.  Overwrite?', dest.path)
  local choice = call('confirm', {msg, '&Force\n&No\n&Rename\n&Time\n&Underbar', 0})
  local ret = ''
  if choice == 1 then
    ret = dest.path
  elseif choice == 2 then
    ret = ''
  elseif choice == 3 then
    -- ('dir' if src.is_dir() else 'file')
    local msg = string.format('%s -> ', src.path)
    ret = call('input', {msg, dest.path, 'file'})
  elseif choice == 4 and d_mtime < s_mtime then
    ret = src.path
  elseif choice == 5 then
    ret = dest.path .. '_'
  end

  -- TODO: notify ret to server --
  rpcrequest('function', {"paste", {pos, src.path, ret}}, true)
end

function M.pre_remove(bufnr, info)
  -- print(vim.inspect(info))
  local msg = string.format('Are you sure to remove %d files?', info.cnt)
  local choice = call('confirm', {msg, '&Yes\n&No\n&Cancel', 0})

  if choice == 1 then
    rpcrequest('function', {"remove", {bufnr, choice}}, true)
  end
end

function M.buf_attach(buf)
  vim.api.nvim_buf_attach(buf, false, { on_detach = function()
    rpcrequest('function', {"on_detach", buf}, true)
  end })
end

-------------------- start of util.vim --------------------
function M.string(expr)
  if type(expr)=='string' then
    return expr
  else
    return vim.fn.string(expr)
  end
end
function M.print_error(s)
  api.nvim_command(string.format("echohl Error | echomsg '[tree] %s' | echohl None", M.string(s)))
end

function __expand(path)
  if path:find('^~') then
    path = vim.fn.fnamemodify(path, ':p')
  end
  return __substitute_path_separator(path)
end
function M.keymap(lhs, ...)
  local args = {...}
  for i, a in ipairs(args) do
    -- print(i, vim.inspect(a))
    if type(a)=='table' then

    end
  end
  -- call_async_action(action, args)
  vim.api.nvim_set_keymap('n', '<c-k>', '<cmd>lua print("hell")<CR>', {nowait=true})
  -- print('----------')
end
function __remove_quote_pairs(s)
  -- remove leading/ending quote pairs
  local t = s
  if (t[1] == '"' and t[#t] == '"') or (t[1] == "'" and t[#t] == "'") then
    t = t:sub(2, #t-1)
  else
    t = vim.fn.substitute(s, [[\\\(.\)]], "\\1", 'g')
  end
  return t
end
function __substitute_path_separator(path)
  if is_windows then
    return vim.fn.substitute(path, '\\', '/', 'g')
  else
    return path
  end
end
function map_filter(func, t)
  vim.validate{func={func,'c'},t={t,'t'}}

  local rettab = {}
  for k, v in pairs(t) do
    if func(k, v) then
      rettab[k] = v
    end
  end
  return rettab
end
function complete(arglead, cmdline, cursorpos)
  local copy = vim.fn.copy
  local _ = {}

  if arglead:find('^-') then
    -- Option names completion.
    local bool_options = vim.tbl_keys(map_filter(
      function(k, v) return type(v) == 'boolean' end, copy(user_options())))
    local bt = vim.tbl_map(function(v) return '-' .. vim.fn.tr(v, '_', '-') end, copy(bool_options))
    vim.list_extend(_, bt)
    local string_options = vim.tbl_keys(map_filter(
      function(k, v) return type(v) ~= type(true) end, copy(user_options())))
    local st = vim.tbl_map(function(v) return '-' .. vim.fn.tr(v, '_', '-') .. '=' end, copy(string_options))
    vim.list_extend(_, st)

    -- Add "-no-" option names completion.
    local nt = vim.tbl_map(function(v) return '-no-' .. vim.fn.tr(v, '_', '-') end, copy(bool_options))
    vim.list_extend(_, nt)
  else
    local al = vim.fn['tree#util#__expand_complete'](arglead)
    -- Path names completion.
    local files = vim.tbl_filter(function(v) return vim.fn.stridx(v:lower(), al:lower()) == 0 end,
      vim.tbl_map(function(v) return __substitute_path_separator(v) end, vim.fn.glob(arglead .. '*', true, true)))
    files = vim.tbl_map(
      function(v) return vim.fn['tree#util#__expand_complete'](v) end,
      vim.tbl_filter(function(v) return vim.fn.isdirectory(v)==1 end, files))
    if arglead:find('^~') then
      local home_pattern = '^'.. vim.fn['tree#util#__expand_complete']('~')
      files = vim.tbl_map(function(v) return vim.fn.substitute(v, home_pattern, '~/', '') end, files)
    end
    files = vim.tbl_map(function(v) return vim.fn.escape(v..'/', ' \\') end, files)
    vim.list_extend(_, files)
  end

  return vim.fn.uniq(vim.fn.sort(vim.tbl_filter(function(v) return vim.fn.stridx(v, arglead) == 0 end, _)))
end
-- Test case
-- -columns=mark:git:indent:icon:filename:size:time -winwidth=40 -listed `expand('%:p:h')`
-- -buffer-name=\`foo\` -split=vertical -direction=topleft -winwidth=40 -listed `expand('%:p:h')`
function __eval_cmdline(cmdline)
  local cl = ''
  local prev_match = 0
  local eval_pos = vim.fn.match(cmdline, [[\\\@<!`.\{-}\\\@<!`]])
  while eval_pos >= 0 do
    if eval_pos - prev_match > 0 then
      cl = cl .. cmdline:sub(prev_match+1, eval_pos)
    end
    prev_match = vim.fn.matchend(cmdline, [[\\\@<!`.\{-}\\\@<!`]], eval_pos)
    cl = cl .. vim.fn.escape(vim.fn.eval(cmdline:sub(eval_pos+2, prev_match-1)), [[\ ]])

    eval_pos = vim.fn.match(cmdline, [[\\\@<!`.\{-}\\\@<!`]], prev_match)
  end
  if prev_match >= 0 then
    cl = cl .. cmdline:sub(prev_match+1)
  end

  return cl
end
function M.new_file(args)
  print(inspect(args))
  ret = fn.input(args.prompt, args.text, args.completion)
  print(ret)
  rpcrequest('function', {"new_file", {ret, args.bufnr}}, true)
end
function M.rename(args)
  print(inspect(args))
  ret = fn.input(args.prompt, args.text, args.completion)
  print(ret)
  rpcrequest('function', {"rename", {ret, args.bufnr}}, true)
end
function M.error(str)
  local cmd = string.format('echomsg "[tree] %s"', str)
  vim.api.nvim_command('echohl Error')
  vim.api.nvim_command(cmd)
  vim.api.nvim_command('echohl None')
end
function M.warning(str)
  local cmd = string.format('echomsg "[tree] %s"', str)
  vim.api.nvim_command('echohl WarningMsg')
  vim.api.nvim_command(cmd)
  vim.api.nvim_command('echohl None')
end
function M.print_message(str)
  local cmd = string.format('echo "[tree] %s"', str)
  vim.api.nvim_command(cmd)
end

local function check_channel()
  return fn.exists('g:tree#_channel_id')
end
function rpcrequest(method, args, is_async)
  if check_channel() == 0 then
    -- TODO: temporary
    M.error("g:tree#_channel_id doesn't exists")
    return -1
  end

  local channel_id = vim.g['tree#_channel_id']
  if is_async then
    return vim.rpcnotify(channel_id, method, args)
  else
    return vim.rpcrequest(channel_id, method, args)
  end
end

function M.linux()
  return is_linux
end
function M.windows()
  return is_windows
end
function M.macos()
  return is_macos
end
-- Open a file.
function M.open(filename)
  local filename = vim.fn.fnamemodify(filename, ':p')
  local system = vim.fn.system
  local shellescape = vim.fn.shellescape
  local executable = vim.fn.executable
  local exists = vim.fn.exists
  local printf = string.format

  -- Detect desktop environment.
  if tree.windows() then
    -- For URI only.
    -- Note:
    --   # and % required to be escaped (:help cmdline-special)
    vim.api.nvim_command(
      printf("silent execute '!start rundll32 url.dll,FileProtocolHandler %s'", vim.fn.escape(filename, '#%')))
  elseif vim.fn.has('win32unix')==1 then
    -- Cygwin.
    system(printf('cygstart %s', shellescape(filename)))
  elseif executable('xdg-open')==1 then
    -- Linux.
    system(printf('%s %s &', 'xdg-open', shellescape(filename)))
  elseif exists('$KDE_FULL_SESSION')==1 and vim.env['KDE_FULL_SESSION'] == 'true' then
    -- KDE.
    system(printf('%s %s &', 'kioclient exec', shellescape(filename)))
  elseif exists('$GNOME_DESKTOP_SESSION_ID')==1 then
    -- GNOME.
    system(printf('gnome-open %s &', shellescape(filename)))
  elseif executable('exo-open')==1 then
    -- Xfce.
    system(printf('exo-open %s &', shellescape(filename)))
  elseif tree.macos() and executable('open')==1 then
    -- Mac OS.
    system(printf('open %s &', shellescape(filename)))
  else
    -- Give up.
    M.print_error('Not supported.')
  end
end
-------------------- end of util.vim --------------------


-------------------- start of init.vim --------------------
-- cant work in lua script
-- print(vim.fn.expand('<sfile>'))
local function init_channel()
  if fn.has('nvim-0.5') == 0 then
    print('tree requires nvim 0.5+.')
    return true
  end

  local servername = vim.v.servername
  local cmd
  -- NOTE: ~ cant expand in {cmd} arg of jobstart
  if M.linux() then
    cmd = {project_root .. '/bin/tree', servername}
  elseif M.windows() then
    cmd = {project_root .. '\\bin\\tree-nvim.exe', '--server', servername}
  elseif M.macos() then
    cmd = {project_root .. '/bin/tree', servername}
  end
  -- print('bin:', bin)
  -- print('servername:', servername)
  -- print(inspect(cmd))
  fn.jobstart(cmd)
  local N = 15
  local i = 0
  while i < N and fn.exists('g:tree#_channel_id') == 0 do
    C('sleep 4m')
    i = i + 1
  end
  -- print(string.format('Wait for server %dms', i*4))
  return true
end

local function initialize()
  if fn.exists('g:tree#_channel_id') == 1 then
    return
  end

  init_channel()
  -- NOTE: Exec VimL snippets in lua.
  vim.api.nvim_exec([[
    augroup tree
      autocmd!
    augroup END
  ]], false)

  -- TODO: g:tree#_histories
  M.tree_histories = {}
end

local function user_var_options()
  return {
    wincol=math.modf(vim.o.columns/4),
    winrow=math.modf(vim.o.lines/3)
  }
end
function user_options()
  return vim.tbl_extend('force', {
    auto_cd=false,
    auto_recursive_level=0,
    buffer_name='default',
    columns='mark:indent:icon:filename:size',
    direction='',
    ignored_files='.*',
    listed=false,
    new=false,
    profile=false,
    resume=false,
    root_marker='[in]: ',
    search='',
    session_file='',
    show_ignored_files=false,
    split='no',
    sort='filename',
    toggle=false,
    winheight=30,
    winrelative='editor',
    winwidth=90,
  }, user_var_options())
end

local function custom_get()
  if not M.custom then
    M.custom = {
      column = {},
      option = {},
      source = {},
    }
  end
  return M.custom
end

local function internal_options()
  return {
    cursor=fn.line('.'),
    drives={},
    prev_bufnr=fn.bufnr('%'),
    prev_winid=fn.win_getid(),
    visual_start=fn.getpos("'<")[2],
    visual_end=fn.getpos("'>")[2],
  }
end
-- 一些设置没有必要传输, action_ctx/setting_ctx
local function init_context(user_context)
  local buffer_name = user_context.buffer_name or 'default'
  local context = user_var_options()
  local custom = custom_get()
  if custom.option._ then
    context = vim.tbl_extend('force', context, custom.option._)
    custom.option._ = nil
  end
  if custom.option.buffer_name then
    context = vim.tbl_extend('force', context, custom.option.buffer_name)
  end
  context = vim.tbl_extend('force', context, user_context)
  -- TODO: support custom#column
  context.custom = custom
  return context
end

local function action_context()
  local context = internal_options()
  return context
end

-------------------- end of init.vim --------------------

-------------------- start of custom.vim --------------------
-- 用name:value或dict扩展dest table
local function set_custom(dest, name_or_dict, value)
  if type(name_or_dict) == 'table' then
    dest = vim.tbl_extend('force', dest, name_or_dict)
  else
    dest[name_or_dict] = value
  end
  return dest
end

function M.custom_column(column_name, name_or_dict, ...)
  local custom = custom_get().column

  for i, key in ipairs(vim.split(column_name, '%s*,%s*')) do
    if not custom[key] then
      custom[key] = {}
    end
    custom[key] = set_custom(custom[key], name_or_dict, ...)
  end
end

function M.custom_option(buffer_name, name_or_dict, ...)
  local custom = custom_get().option

  for i, key in ipairs(vim.split(buffer_name, '%s*,%s*')) do
    if not custom[key] then
      custom[key] = {}
    end
    custom[key] = set_custom(custom[key], name_or_dict, ...)
  end
end

function M.custom_source(source_name, name_or_dict, ...)
  local custom = custom_get().source

  for i, key in ipairs(fn.split(source_name, [[\s*,\s*]])) do
    if not custom[key] then
      custom[key] = {}
    end
    custom[key] = set_custom(custom[key], name_or_dict, ...)
  end
end
-------------------- end of custom.vim --------------------

-------------------- start of tree.vim --------------------
function start(paths, user_context)
  initialize()
  local context = init_context(user_context)
  local paths = fn.map(paths, "fnamemodify(v:val, ':p')")
  if #paths == 0 then
    paths = {fn.expand('%:p:h')}
  end
  rpcrequest('_tree_start', {paths, context}, false)
  -- TODO: 检查 search 是否存在
  -- if context['search'] !=# ''
  --   call tree#call_action('search', [context['search']])
  -- endif
end
function M.action(action, ...)
  if vim.bo.filetype ~= 'tree' then
    return ''
  end
  local args = {...}
  args = args[1] or {}
  if not vim.tbl_islist(args) then
    args = {args}
  end
  return api.nvim_eval(string.format([[":\<C-u>call v:lua.call_async_action(%s, %s)\<CR>"]],
         fn.string(action), fn.string(args)))
end

function M.call_action(action, ...)
  if vim.bo.filetype ~= 'tree' then
    return
  end

  local context = action_context()
  local args = ...
  if type(args) ~= type({}) then
    args = {...}
  end
  rpcrequest('_tree_do_action', {action, args, context}, false)
end
function call_async_action(action, ...)
  if vim.bo.filetype ~= 'tree' then
    return
  end

  local context = action_context()
  local args = ...
  if type(args) ~= type({}) then
    args = {...}
  end
  rpcrequest('_tree_async_action', {action, args, context}, true)
end

function M.get_candidate()
  if vim.bo.filetype ~= 'tree' then
    return {}
  end

  local context = internal_options()
  return rpcrequest('_tree_get_candidate', {context}, false)
end
function M.is_directory()
  return fn.get(M.get_candidate(), 'is_directory', false)
end
function M.is_opened_tree()
  return fn.get(M.get_candidate(), 'is_opened_tree', false)
end

function M.get_context()
  if vim.bo.filetype ~= 'tree' then
    return {}
  end

  return rpcrequest('_tree_get_context', {}, false)
end
-------------------- end of tree.vim --------------------
function M.refactor(old)
  local C = vim.api.nvim_command
  local msg = 'Rename to: '
  local new = fn.input(msg, '', 'file')
  local cmd = string.format(':%%s/\\<%s\\>/%s/gIc', old, new)
  print(cmd)
  C(cmd)
end

function M.rrequire(module)
  package.loaded[module] = nil
  return require(module)
end

if _TEST then
  -- Note: we prefix it with an underscore, such that the test function and real function have
  -- different names. Otherwise an accidental call in the code to `M.FirstToUpper` would
  -- succeed in tests, but later fail unexpectedly in production
  M._set_custom = set_custom
  M._init_context = init_context
  M._initialize = initialize
end

return M
