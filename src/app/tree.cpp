#include <boost/filesystem.hpp>
#include <cinttypes>
#include <cwchar>
#include <codecvt>
#include "tree.h"

#if defined(Q_OS_WIN)
extern int mk_wcwidth(wchar_t ucs);
#endif
using namespace boost::filesystem;
using std::string;
using std::cout;
using std::endl;

Tree::ClipboardMode Tree::paste_mode;
list<string> Tree::clipboard;
nvim::Nvim *Tree::api;

Tree::~Tree()
{
    erase_entrylist(0, m_fileitem.size());
}
Tree::Tree(int bufnr, int ns_id)
    : bufnr(bufnr), icon_ns_id(ns_id)
{

    api->buf_set_option(bufnr, "ft", "tree");
    api->buf_set_option(bufnr, "modifiable", false);

    api->command("lua tree = require('tree')");
    // b->buf_attach(bufnr, false, {});
    api->execute_lua("tree.buf_attach(...)", {bufnr});
}

int countgrid(const std::wstring &s)
{
    int n = s.size();
    int ans = 0;
    for (int i = 0; i < n; i++) {
        const wchar_t wc = s.at(i);

        // NOTE: wcwidth need unicode
#if defined(Q_OS_WIN)
        if (mk_wcwidth(wc)==2) {
            ans++;
        }
#else
        if (wcwidth(wc)==2) {
            ans++;
        }
#endif
    }
    return n + ans;  // screen cells
}
void truncate(const int l, Cell &cell)
{
    int old_bytesize = cell.text.size();
    int n = cell.col_end - cell.col_start - l;
    int total = 0; // total visual width
    int pos = 1;
    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;
    std::wstring str = converter.from_bytes(cell.text.c_str());
    while (total <= n) {
#if defined(Q_OS_WIN)
        total += mk_wcwidth(str.at(pos));
#else
        total += wcwidth(str.at(pos));
#endif
        str.erase(pos, 1);
    }
    str.insert(pos, L"…");
    // https://stackoverflow.com/questions/4804298/how-to-convert-wstring-into-string
    // use converter (.to_bytes: wstr->str, .from_bytes: str->wstr)
    cell.text = converter.to_bytes(str);
    cell.byte_end -= old_bytesize - cell.text.size();
    cell.col_end -= total - 1;
}

void Tree::makeline(const int pos, string &line)
{
    assert(0<=pos&&pos<col_map[FILENAME].size());
    int start = 0;
    for (int col : cfg.columns) {
        const Cell & cell = col_map[col][pos];
        line.append(string(cell.col_start-start, ' '));
        line.append(cell.text);
        int len = cell.byte_end - cell.byte_start - cell.text.size();
        // string cell_str(cell.text);
        // if (col=="filename")
        //     len = cell.col_end-countgrid(cell_str)-cell.col_start;
        // else
        //     len = cell.col_end-cell_str.size()-cell.col_start;
        string spc_after(len, ' ');
        line.append(std::move(spc_after));
        start = cell.col_end;
    }
    // cout<<"mkline"<<pos<<":"<<line;
}
void Tree::changeRoot(const string &root)
{
    // TODO: cursor history
    path dir(root);
    if (!exists(dir)) {
        return;
    }
    const string & rootPath = dir.string();
    expandStore.insert({rootPath, true});

    FileItem::update_gmap(root);
    targets.clear();
    erase_entrylist(0, m_fileitem.size());

    FileItem *fileitem = new FileItem;
    fileitem->level = -1;
    fileitem->opened_tree = true;
    fileitem->p = dir;
    fileitem->filename = rootPath;
    m_fileitem.push_back(fileitem);

    insert_rootcell(0);
    // FIXME: when icon not available
    // col_map["icon"][0].text = "";

    vector<string> ret;
    string line;
    makeline(0, line);
    ret.push_back(line);

    vector<FileItem*> child_fileitem;
    entryInfoListRecursively(*m_fileitem[0], child_fileitem);
    for (auto item:child_fileitem) {
        m_fileitem.push_back(item);
    }

    insert_entrylist(child_fileitem, 1, ret);

    buf_set_lines(0, -1, true, ret);
    string k = (*m_fileitem[0]).p.string();
    // if (cursorHistory.contains(k)) {
    //     api->win_set_cursor(0, QPoint{0, cursorHistory[k]});
    // }

    hline(0, m_fileitem.size());
}

/// Insert columns
void Tree::insert_item(const int pos)
{
    const FileItem &fileitem = *m_fileitem[pos];
    int start = 0;
    int byte_start = 0;
    const int kStop = cfg.filename_colstop;
    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;
    for (const int col : cfg.columns) {
        Cell cell(cfg, fileitem, col);
        std::wstring ws = converter.from_bytes(cell.text.c_str());
        cell.byte_start = byte_start;
        cell.byte_end = byte_start+cell.text.size();
        cell.col_start = start;
        if (col==FILENAME) {
            cell.col_end = start + countgrid(ws);
        }
        else {
            cell.col_end = start + ws.size();
        }

        // NOTE: alignment
        if (col==FILENAME) {
            int tmp = kStop - cell.col_end;
            // TODO:此处都设置成统一列，在makeline时进行截断
            if (tmp > 0) {
                cell.col_end+=tmp;
                cell.byte_end+=tmp;
            }
            else if (kStop>cell.col_start+5) {
                truncate(kStop - cell.col_start, cell);
                int tmp = kStop - cell.col_end;
                if (tmp > 0) {
                    cell.col_end+=tmp;
                    cell.byte_end+=tmp;
                }
            }
        }

        int sep = (col==INDENT?0:1);
        start = cell.col_end + sep;
        byte_start = cell.byte_end + sep;

        auto search = col_map.find(col);
        if (search != col_map.end()) {
            col_map[col].emplace(search->second.begin()+pos, std::move(cell));
        } else {
            col_map.insert({col, vector<Cell>()});
        }
    }
}

void Tree::insert_rootcell(const int pos)
{
    const FileItem &fileitem = *m_fileitem[pos];
    int start = 0;
    int byte_start = 0;
    const int kStop = cfg.filename_colstop;
    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;
    for (int col : cfg.columns) {
        Cell cell(cfg, fileitem, col);
        cell.col_start = start;
        cell.byte_start = byte_start;
        if (col==FILENAME) {
            string filename(fileitem.filename);
            if (filename.back() != '/' && is_directory(fileitem.p)) {
                filename.append("/");
            }
            filename.insert(0, cfg.root_marker.c_str());
            cell.text = filename;
        }
        std::wstring ws = converter.from_bytes(cell.text.c_str());
        string cell_str(cell.text);
        cell.byte_end = byte_start+cell.text.size();

        if (col==FILENAME) {
            cell.col_end = start + countgrid(ws);
        }
        else {
            cell.col_end = start + ws.size();
        }

        // NOTE: alignment
        if (col==FILENAME) {
            int tmp = kStop - cell.col_end;
            // TODO:此处都设置成统一列，在makeline时进行截断
            if (tmp >0) {
                cell.col_end+=tmp;
                cell.byte_end+=tmp;
            }
        }

        int sep = (col==INDENT?0:1);
        start = cell.col_end + sep;
        byte_start = cell.byte_end + sep;

        auto search = col_map.find(col);
        if (search != col_map.end()) {
            col_map[col].insert(search->second.begin()+pos, std::move(cell));
        } else {
            col_map.insert({col, vector<Cell>()});
        }
    }
}
/// pos is 0-based row number.
void Tree::insert_entrylist(const vector<FileItem*>& fil, const int pos, vector<string>& ret)
{
    int file_count = fil.size();
    for (int i = 0; i < file_count; ++i) {
        insert_item(pos+i);

        string line;
        makeline(pos+i, line);
        ret.push_back(std::move(line));
    }
}
// TODO: 通过FileItem的parent查询
/// l is 0-based row number.
/// NOTE: root.level=-1
int Tree::find_parent(int l)
{
    int level = m_fileitem[l]->level;
    for (int i=l-1;i>=0;--i)
    {
        const FileItem &fn = *m_fileitem[i];
        if (fn.level == level-1)
            return i;
    }
    return -1;
}

/// [l, l] <=> no sub files; l is parent row number(0-based).
std::tuple<int, int> Tree::find_range(int l)
{
    int s=l, i;
    int level = m_fileitem[l]->level;
    for (i=l+1;i<m_fileitem.size();++i)
    {
        int l = m_fileitem[i]->level;
        if (level >= l)
            break;
    }
    return std::make_tuple(s, i-1);
}

/// 0-based [sl, el).
void Tree::hline(int sl, int el)
{
    int bufnr = this->bufnr;
    api->async_buf_clear_namespace(bufnr, icon_ns_id, sl, el);
    for (int i = sl;i<el;++i)
    {
        const FileItem &fileitem = *m_fileitem[i];
        char name[40];

        for (const int col : cfg.columns) {
            const Cell & cell = col_map[col][i];

            if(col==FILENAME) {
                sprintf(name, "tree_%u_%u", col, is_directory(fileitem.p));
                api->async_buf_add_highlight(bufnr, icon_ns_id, name, i, cell.byte_start, cell.byte_start+cell.text.size());
            } else if(col==ICON || col==GIT || col==MARK) {
                // :hi tree_<tab>
                sprintf(name, "tree_%u_%u", col, cell.color);
                // sprintf(name, "tree_%s", cell.text.data());
                // cout << icon.col_start<< "->"<<icon.col_end;
                // cout << icon.text;
                // auto req_hl = api->buf_add_highlight(bufnr, 0, "String", 0, 0, 3);
                // call buf_add_highlight(0, -1, "Identifier", 0, 5, -1)
                api->async_buf_add_highlight(bufnr, icon_ns_id, name, i, cell.byte_start, cell.byte_start+cell.text.size());
            } else if (col==SIZE || col==TIME ){//|| col==INDENT
                sprintf(name, "tree_%u", col);
                api->async_buf_add_highlight(bufnr, icon_ns_id, name, i, cell.byte_start, cell.byte_start+cell.text.size());
            }
        }
    }
}

/// 0-based [sl, el).
void Tree::redraw_line(int sl, int el)
{
    char format[] = "%s (1-based): [%d, %d]\n";
    printf(format, __PRETTY_FUNCTION__, sl+1, el);

    vector<string> ret;
    const int kStop = cfg.filename_colstop;
    for (int i = sl; i < el; ++i) {
        FileItem & fileitem = *m_fileitem[i];

        int start = 0;
        int byte_start = 0;
        for (const int col : cfg.columns) {
            Cell& cell = col_map[col][i];
            if (col==MARK){
            }else if(col==INDENT){
            }else if(col==GIT){
                // TODO: Git::update_gmap(fn);
                cell.update_git(fileitem);
            }else if(col==ICON){
                cell.update_icon(fileitem);
            } else if(col==FILENAME){
            }else if(col==SIZE){
                cell.update_size(fileitem);
            }
            std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;
            std::wstring cell_str = converter.from_bytes(cell.text.c_str());
            cell.col_start =start;
            cell.col_end = start + countgrid(cell_str);
            cell.byte_start = byte_start;
            cell.byte_end = byte_start+cell.text.size();

            // cout << col;
            if (col==FILENAME)
            {
                int tmp = kStop - cell.col_end;
                if (tmp >0)
                {
                    cell.col_end+=tmp;
                    cell.byte_end+=tmp;
                }
            }
            int sep = (col==INDENT?0:1);
            start = cell.col_end + sep;
            byte_start = cell.byte_end + sep;
        }

        string line;
        makeline(i, line);
        ret.push_back(std::move(line));
    }
    buf_set_lines(sl, el, true, std::move(ret));
    hline(sl, el);
}
/// redraw parent and children
/// assert l is 0-based parent index
void Tree::redraw_recursively(int l)
{
    assert(0 <= l && l < m_fileitem.size());
    FileItem &cur = *m_fileitem[l];

    std::tuple<int, int> se = find_range(l);
    int s = std::get<0>(se) + 1;
    int e = std::get<1>(se) + 1;
    printf("%s redraw range(1-based): [%d, %d]", __PRETTY_FUNCTION__, s+1, e);

    erase_entrylist(s, e);

    vector<FileItem*> child_fileitem;
    // const string &p = cur.p.string();
    entryInfoListRecursively(cur, child_fileitem);
    int file_count = child_fileitem.size();
    for (int i = 0; i < file_count; ++i)
        m_fileitem.insert(m_fileitem.begin() + l + 1 + i, child_fileitem[i]);

    if (file_count <= 0) {
        return;
    }

    vector<string> ret;
    insert_entrylist(child_fileitem, l + 1, ret);
    buf_set_lines(s, e, true, ret);
    hline(l + 1, l + 1 + ret.size());

    return;
}
/// erase [s, e)
void Tree::erase_entrylist(const int s, const int e)
{
    for (const int col : cfg.columns) {
        vector<Cell> & cell = col_map[col];
        cell.erase(cell.begin()+s, cell.begin()+e);
    }
    for (int i = s; i < e; ++i) {
        delete m_fileitem[i];
    }
    m_fileitem.erase(m_fileitem.begin()+s, m_fileitem.begin()+e);
}

// get entryInfoList recursively
void Tree::entryInfoListRecursively(const FileItem& item,
                                   vector<FileItem*> &fileitem_lst)
{
    path dir(item.p);
    const int level = item.level+1;
    typedef vector<path> vec;             // store paths,
    vec v;                                // so we can sort them later
    try {
        if (cfg.show_ignored_files)
            copy(directory_iterator(item.p), directory_iterator(), back_inserter(v));
        else
            copy_if(directory_iterator(item.p), directory_iterator(), back_inserter(v),
                [](auto x){return (x.path().filename().string().front() != '.');});
    } catch(std::exception& e) {
        cout << "------->" << e.what() << endl;
        return;
    }
    sort(v.begin(), v.end(), [](path x, path y){return is_directory(x)>is_directory(y);});

    for (auto &x : v) {
      try {
        FileItem *fileitem = new FileItem;
        fileitem->level = level;
        fileitem->parent = &item;
        fileitem->p = x;
        fileitem->filename = x.filename().string();
        if (&x == &(*(v.end()-1))) {
            fileitem->last = true;
        }

        auto search = expandStore.find(fileitem->p.string());
        if (search != expandStore.end() && search->second) {
            fileitem->opened_tree = true;
            fileitem_lst.push_back(fileitem);
            entryInfoListRecursively(*fileitem, fileitem_lst);
        }
        else
            fileitem_lst.push_back(fileitem);
      } catch(std::exception& e) {
          continue;
      }
    }
}

// shrink recursively
void Tree::shrinkRecursively(const string &p)
{
    path dir(p);
    // set_dir(dir);
    typedef vector<path> vec;             // store paths,
    vec v;                                // so we can sort them later
    try {
        copy(directory_iterator(p), directory_iterator(), back_inserter(v));
    } catch(std::exception& e) {
        cout << "------->" << e.what() << endl;
        return;
    }

    for (auto x : v) {
        string p = x.string();

        auto got = expandStore.find(p);
        if (got != expandStore.end() && expandStore[p]) {
            expandStore[p] = false;
            shrinkRecursively(p);
        }
    }
    return;
}

void Tree::expandRecursively(const FileItem &item, vector<FileItem*> &fileitems)
{
    const int level = item.level+1;
    typedef vector<directory_entry> vec;             // store paths,
    vec v;                                // so we can sort them later
    try {
        copy(directory_iterator(item.p), directory_iterator(), back_inserter(v));
    } catch(std::exception& e) {
        cout << "------->" << e.what() << endl;
        return;
    }
    sort(v.begin(), v.end(), [](path x, path y){return is_directory(x)>is_directory(y);});

    for (directory_entry &x : v) {
      try {
        FileItem *fileitem = new FileItem;
        fileitem->level = level;
        fileitem->parent = &item;
        fileitem->p = x.path();
        fileitem->filename = x.path().filename().string();
        if (&x == &(*(v.end()-1))) {
            fileitem->last = true;
        }

        if (is_directory(x)) {
            string p = x.path().string();
            expandStore.insert({p, true});
            fileitem->opened_tree = true;
            fileitems.push_back(fileitem);
            expandRecursively(*fileitem, fileitems);
        }
        else
            fileitems.push_back(fileitem);
      } catch(std::exception& e) {
          continue;
      }
    }
}

void Tree::handleRename(string &input)
{
    cout << __PRETTY_FUNCTION__ << endl;

    Cell & cur = col_map[FILENAME][ctx.cursor-1];
    FileItem & item = *m_fileitem[ctx.cursor-1];
    if (!is_directory(item.p) && input.back() == '/')
        input.pop_back();
    string fn = item.p.string();
    boost::filesystem::rename(item.p, input);
    api->execute_lua("tree.print_message(...)", {"Rename Success"});
    string text(item.p.filename().string());
    if (is_directory(item.p))
        text.append("/");
    cur.text = text;

    // NOTE: gmap may update
    // FileItem::update_gmap(item.fi.absolutePath());
    // redraw_line(ctx.cursor-1, ctx.cursor);
    // TODO: 细粒度redraw
    FileItem &root = *m_fileitem[0];
    changeRoot(root.p.string());

    // api->execute_lua("tree.print_message(...)", {"Rename failed"});

}
void Tree::handleNewFile(const string &input)
{
    if (input=="") {
        api->execute_lua("tree.print_message(...)", {"Canceled"});
        return;
    }
    cout << __PRETTY_FUNCTION__ << input;

    // Cell & cur = col_map["filename"][ctx.cursor-1];
    FileItem & item = *m_fileitem[ctx.cursor-1];

    path dest = item.opened_tree ? item.p : item.p.parent_path();

    dest += path::preferred_separator + input;
    cout << dest << endl;
    // QFileInfo fi(dest.filePath(input));
    // NOTE: failed when same name file exists
    if (boost::filesystem::exists(dest)) {
        api->execute_lua("tree.print_message(...)", {"File already exists!"});
        return;
    }
    else if(input.back() == '/'){
        if(!create_directory(dest))
            api->execute_lua("tree.print_message(...)", {"Failed to create dir!"});
    } else {
        boost::filesystem::ofstream(dest.string());
    }

    if (item.opened_tree) {
        redraw_recursively(ctx.cursor-1);
    } else{
        // TODO: location: find_child
        int pidx = find_parent(ctx.cursor-1);
        // NOTE: root.level=-1
        // TODO: consider named redraw_family
        redraw_recursively(pidx);
    }
}

/// 收集无序targets
/// 视图变化之后 targets 要更新
void Tree::collect_targets()
{
    targets.clear();
    for (int i = 0; i < m_fileitem.size(); ++i) {
        const FileItem &item = *m_fileitem[i];
        if (item.selected) {
            targets.push_back(i);
        }
    }
}
void Tree::paste(const int ln, const string &src, const string &dest)
{
    if (is_directory(src)) {
        if (paste_mode == COPY) {
            copy(src, dest);
            api->async_execute_lua("tree.print_message(...)", {"Copyed"});
            cout<<"Copy Paste dir"<<endl;
            int pidx = find_parent(ln);
            redraw_recursively(pidx);
        }
        else if (paste_mode == MOVE){
            boost::filesystem::rename(src, dest);
            cout<<"Move Paste dir"<<endl;
            FileItem &root = *m_fileitem[0];
            changeRoot(root.p.string());
        }
    }
    else {
        if (paste_mode == COPY) {
            copy(src, dest);
            api->async_execute_lua("tree.print_message(...)", {"Copyed"});
            cout<<"Copy Paste"<<endl;
            int pidx = find_parent(ln);
            redraw_recursively(pidx);
        }
        else if (paste_mode == MOVE){
            boost::filesystem::rename(src, dest);
            cout<<"Move Paste"<<endl;
            FileItem &root = *m_fileitem[0];
            changeRoot(root.p.string());
        }
    }
    return;
}
typedef void (Tree::*Action)(const nvim::Array& args);
std::unordered_map<string, Action> action_map {
    {"cd"                   , &Tree::cd},
    {"goto"                 , &Tree::goto_},
    {"open_or_close_tree"   , &Tree::open_tree},
    {"open"                 , &Tree::open},
    {"copy"                 , &Tree::copy_},
    {"move"                 , &Tree::move},
    {"paste"                , &Tree::pre_paste},
    {"remove"               , &Tree::pre_remove},
    {"yank_path"            , &Tree::yank_path},
    {"toggle_select"        , &Tree::toggle_select},
    {"toggle_select_all"    , &Tree::toggle_select_all},
    {"print"                , &Tree::print},
    {"debug"                , &Tree::debug},
    {"toggle_ignored_files" , &Tree::toggle_ignored_files},
    {"redraw"               , &Tree::redraw},
    {"new_file"             , &Tree::new_file},
    {"execute_system"       , &Tree::execute_system},
    {"rename"               , &Tree::rename},
    {"drop"                 , &Tree::drop},
    {"call"                 , &Tree::call},
    {"open_tree_recursive"  , &Tree::open_or_close_tree_recursively},
};
void Tree::action(const string &action, const nvim::Array &args,
                  const Map &context)
{
    cout << __FUNCTION__ << ":" << action << endl;

    this->ctx = context;
    cout << "cursor position(1-based): " << ctx.cursor << endl;

    auto search = action_map.find(action);
    if (search != action_map.end()) {
        (this->*action_map[action])(args);
    }
    else {
        api->call_function("tree#util#print_message", {"Unknown Action: " + action});
    }
}

/// l is 0-based row number.
void Tree::open_tree(const nvim::Array &args)
{
    const int l = ctx.cursor - 1;
    cout << __FUNCTION__ << endl;
    assert(0 <= l && l < m_fileitem.size());
    // if (l == 0) return;
    vector<string> ret;
    FileItem &cur = *m_fileitem[l];

    if (is_directory(cur.p) && !cur.opened_tree) {

        cur.opened_tree = true;
        const string & rootPath = cur.p.string();
        expandStore[rootPath] = true;
        redraw_line(l, l + 1);
        vector<FileItem*> child_fileitem;
        entryInfoListRecursively(cur, child_fileitem);
        int file_count = child_fileitem.size();
        for (int i=0;i<file_count;++i) {
            m_fileitem.insert(m_fileitem.begin()+l+1+i, child_fileitem[i]);
        }

        if (file_count <= 0) {
            return;
        }

        insert_entrylist(child_fileitem, l + 1, ret);

        buf_set_lines(l+1, l+1, true, ret);
        hline(l + 1, l + 1 + ret.size());
    }
    else if (cur.opened_tree) {
        const string & p = cur.p.string();
        auto search = expandStore.find(p);
        if (search != expandStore.end() && expandStore[p]) {
            expandStore[p] = false;
        }
        std::tuple<int, int> se = find_range(l);
        int s = std::get<0>(se) + 1;
        int e = std::get<1>(se) + 1;
        printf("\tclose range(1-based): [%d, %d]\n", s+1, e);
        buf_set_lines(s, e, true, {});

        erase_entrylist(s, e);
        cur.opened_tree = false;
        redraw_line(l, l + 1);
    }
    else if (find_parent(l) >= 0) {
        int parent = find_parent(l);
        std::tuple<int, int> se = find_range(parent);
        int s = std::get<0>(se) + 1;
        int e = std::get<1>(se) + 1;

        printf("\tclose range(1-based): [%d, %d]\n", s+1, e);

        buf_set_lines(s, e, true, {});
        // ref to https://github.com/equalsraf/neovim-qt/issues/596
        api->async_win_set_cursor(0, {s, 0});
        erase_entrylist(s, e);

        FileItem &father = *m_fileitem[parent];
        father.opened_tree = false;
        const string & p = father.p.string();
        auto got = expandStore.find(p);
        if (got != expandStore.end() && expandStore[p]) {
            expandStore[p] = false;
        }
        redraw_line(parent, parent + 1);
    }
    return;
}

Map Tree::get_candidate(const int pos)
{
    // 'word': 'column.cpp',
    FileItem & item = *m_fileitem[pos];
    return {
        {"is_directory", is_directory(item.p)},
        {"action__path", item.p.string()},
        {"level", item.level},
        {"is_opened_tree", item.opened_tree},
        {"is_selected", item.selected}
    };
}
void Tree::open(const nvim::Array &args)
{
    //save_cursor();
    const int l = ctx.cursor - 1;
    const path &p = m_fileitem[l]->p;
    if (is_directory(p)) {
        changeRoot(p.string());
    }
    else if (args.size()>0 && args[0].as_string()=="vsplit") {
        cout << "vsplit " << p;
        api->async_call_function("tree#util#execute_path", {"rightbelow vsplit", p.string()});
    }
    else {
        api->async_call_function("tree#util#execute_path", {"drop", p.string()});
    }
}

void Tree::rename(const nvim::Array &args)
{
    // cout << action << args;
    FileItem &cur = *m_fileitem[ctx.cursor - 1];
    string info = cur.p.string();
    nvim::Dictionary cfg{
        {"prompt", "Rename: " + info + " -> "},
        {"text", info},
        {"completion", "file"},
        {"handle", "rename"},
        {"bufnr", bufnr}
    };
    api->async_execute_lua("tree.rename(...)", {cfg});
}

void Tree::drop(const nvim::Array &args)
{
    FileItem &cur = *m_fileitem[ctx.cursor - 1];
    const auto p = cur.p;
    // cout<<args;
    if (is_directory(cur.p))
        changeRoot(p.string());
    else {
        api->execute_lua("tree.drop(...)", {args, p.string()});
    }
}
void Tree::cd(const nvim::Array &args)
{
    if (args.size()>0) {
        string dir = args.at(0).as_string();

        // FIXME 第一次cd无效
        if (dir=="..") {
            path & curdir = m_fileitem[0]->p;
            cout << __FUNCTION__ << curdir.parent_path().string() << endl;
            changeRoot(curdir.parent_path().string());
        }
        else if (dir == ".") {
            FileItem &cur = *m_fileitem[ctx.cursor - 1];
            string dir(cur.p.string());
            // if (cur.opened_tree)
            //     dir = cur.fi.absoluteFilePath();
            string cmd = "cd " + dir;
            api->async_execute_lua("tree.print_message(...)", {cmd});
            api->async_command(cmd);
        }
        else {
            changeRoot(dir);
        }
    }
    else {
        // changeRoot(QDir::home().absolutePath());
    }
}
void Tree::call(const nvim::Array &args)
{
    string func = args.at(0).as_string();
    FileItem &cur = *m_fileitem[ctx.cursor - 1];
    Map ctx = {
        {"targets", cur.p.string()}
    };
    api->async_call_function(func, {ctx});
}

void Tree::print(const nvim::Array &args)
{
    FileItem &cur = *m_fileitem[ctx.cursor - 1];
    string msg = cur.p.string();
    string msg2 = "last=" + string(cur.last ? "true" : "false");
    string msg3 = "level=" + std::to_string(cur.level);
    api->async_execute_lua("tree.print_message(...)", {msg+" "+msg2+" "+msg3});
}
void Tree::pre_paste(const nvim::Array &args)
{
    // TODO: 批量paste
    if (clipboard.size() <= 0) {
        api->async_execute_lua("tree.print_message(...)", {"Nothing in clipboard"});
        return;
    }
    for (const string &f : clipboard) {
        FileItem &cur = *m_fileitem[ctx.cursor - 1];
        string fname = path(f).filename().string();
        path curdir = cur.p.parent_path();
        if (cur.opened_tree) curdir = cur.p.string();
        string destfile = curdir.string() + path::preferred_separator + fname;
        cout << "destfile:" << destfile << endl;
        cout << "fname:" << fname << endl;
        if (exists(destfile)) {
            // api->async_execute_lua("tree.print_message(...)", {"Destination file exists"});

            Map dest {
                {"mtime", last_write_time(destfile)},
                {"path", destfile},
                {"size", file_size(destfile)},
            };

            Map src {
                {"mtime", last_write_time(f)},
                {"path", f},
                {"size", file_size(f)},
            };
            api->async_execute_lua("tree.pre_paste(...)",
                {nvim::Array{bufnr, ctx.cursor - 1}, dest, src});
        }
        else {
            paste(ctx.cursor-1, f, destfile);
        }
    }
}

void Tree::debug(const nvim::Array &args)
{
    for (auto i : cfg.columns) {
        cout << i << ":";
    }
    cout << endl;
    for (auto i : expandStore) {
        cout << i.first << ":" << i.second << endl;
    }
    for (auto i : FileItem::git_map) {
        cout << i.first << ":" << i.second << endl;
    }
    FileItem &cur = *m_fileitem[ctx.cursor - 1];
}
void Tree::yank_path(const nvim::Array &args)
{
    vector<string> yank;
    for (const int &pos : targets) {
        yank.push_back(m_fileitem[pos]->p.string());
    }
    if (yank.size()==0) {
        FileItem &cur = *m_fileitem[ctx.cursor - 1];
        yank.push_back(cur.p.string());
    }
    string reg;
    for (auto i : yank) {
        reg += i;
        reg += '\n';
    }
    api->call_function("setreg", {"\"", reg});

    reg.insert(0, "yank_path\n");
    api->execute_lua("tree.print_message(...)", {reg});
}
void Tree::pre_remove(const nvim::Array &args)
{
    int cnt = targets.size();
    Map farg;
    farg.insert({"cnt", cnt==0 ? 1:cnt});
    api->async_execute_lua("tree.pre_remove(...)", {bufnr, farg});
}
void Tree::remove()
{
    // TODO: remove 之后光标位置
    vector<string> rmfiles;
    for (const int &pos : targets) {
        rmfiles.push_back(m_fileitem[pos]->p.string());
    }
    if (rmfiles.size()==0) {
        FileItem &cur = *m_fileitem[ctx.cursor - 1];
        rmfiles.push_back(cur.p.string());
    }
    for (const string &f : rmfiles) {
        cout << f << endl;
        if (is_directory(f))
            boost::filesystem::remove_all(f);
        else
            boost::filesystem::remove(f);
    }
    FileItem &root = *m_fileitem[0];
    changeRoot(root.p.string());
}
void Tree::redraw(const nvim::Array &args)
{
    FileItem &root = *m_fileitem[0];
    changeRoot(root.p.string());
}
void Tree::new_file(const nvim::Array &args)
{
    FileItem &cur = *m_fileitem[ctx.cursor - 1];
    string info = cur.p.string();
    nvim::Dictionary cfg{
        {"prompt", "New File: " + info + "/"},
        {"text", ""},
        {"completion", "file"},
        {"handle", "new_file"},
        {"bufnr", bufnr}
    };
    api->async_execute_lua("tree.new_file(...)", {cfg});
}
void Tree::_toggle_select(const int pos)
{
    // TODO: mark may not available
    Cell &cur = col_map[MARK][pos];
    FileItem& item = *m_fileitem[pos];

    item.selected = !item.selected;
    if (item.selected) {
        cur.text = mark_indicators["selected_icon"];
        cur.color = BLUE;
        targets.push_back(pos);
    }
    else {
        cur.text=" ";
        cur.color = WHITE;
        targets.remove(pos);
    }

    redraw_line(pos, pos+1);
}
void Tree::toggle_select(const nvim::Array &args)
{
    const int pos = ctx.cursor - 1;
    _toggle_select(pos);
}
void Tree::toggle_select_all(const nvim::Array &args)
{
    for (int i=1;i<m_fileitem.size();++i) {
        _toggle_select(i);
    }
}
void Tree::_copy_or_move(const nvim::Array &args)
{
    Tree::clipboard.clear();

    for (const int &pos : targets) {
        const FileItem *p = m_fileitem[pos]->parent;
        // NOTE: root item or parent selected
        if (p==nullptr || !p->selected)
            Tree::clipboard.push_back(m_fileitem[pos]->p.string()) ;
    }
    if (clipboard.size()==0) {
        FileItem &cur = *m_fileitem[ctx.cursor - 1];
        Tree::clipboard.push_back(cur.p.string());
    }
    for (auto i:clipboard)
        cout << i << endl;
}

void Tree::copy_(const nvim::Array &args)
{
    paste_mode = COPY;
    _copy_or_move(args);
    api->async_execute_lua("tree.print_message(...)", {"Copy to clipboard"});
}
void Tree::move(const nvim::Array &args)
{
    paste_mode = MOVE;
    _copy_or_move(args);
    api->async_execute_lua("tree.print_message(...)", {"Move to clipboard"});
}
void Tree::open_or_close_tree_recursively(const nvim::Array &args)
{
    const int l = ctx.cursor - 1;
    cout << __PRETTY_FUNCTION__;
    assert(0 <= l && l < m_fileitem.size());
    if (l == 0) return;
    vector<string> ret;
    FileItem &cur = *m_fileitem[l];

    if (is_directory(cur.p) && !cur.opened_tree) {
        cur.opened_tree = true;
        const string &rootPath = cur.p.string();
        expandStore.insert({rootPath, true});
        redraw_line(l, l + 1);
        vector<FileItem*> child_fileitem;
        expandRecursively(cur, child_fileitem);
        int file_count = child_fileitem.size();
        for (int i = 0; i < file_count; ++i)
            m_fileitem.insert(m_fileitem.begin() + l + 1 + i, child_fileitem[i]);

        if (file_count <= 0) {
            return;
        }

        insert_entrylist(child_fileitem, l + 1, ret);
        buf_set_lines(l+1, l+1, true, ret);
        hline(l + 1, l + 1 + ret.size());
        return;
    }
    else if (cur.opened_tree) {
        const string &p = cur.p.string();
        auto got = expandStore.find(p);
        if (got != expandStore.end() && expandStore[p]) {
            expandStore[p] = false;
        }
        std::tuple<int, int> se = find_range(l);
        int s = std::get<0>(se) + 1;
        int e = std::get<1>(se) + 1;
        printf("\tclose range(1-based): [%d, %d]\n", s+1, e);
        buf_set_lines(s, e, true, {});
        shrinkRecursively(p);
        erase_entrylist(s, e);
        cur.opened_tree = false;
        redraw_line(l, l + 1);
        return;
    }
    else if (find_parent(l) >= 0) {
        int parent = find_parent(l);
        std::tuple<int, int> se = find_range(parent);
        int s = std::get<0>(se) + 1;
        int e = std::get<1>(se) + 1;

        printf("\tclose range(1-based): [%d, %d]\n", s+1, e);

        buf_set_lines(s, e, true, {});
        // ref to https://github.com/equalsraf/neovim-qt/issues/596
        api->async_win_set_cursor(0, {s, 0});

        FileItem &father = *m_fileitem[parent];
        father.opened_tree = false;
        const string &p = father.p.string();
        auto got = expandStore.find(p);
        if (got != expandStore.end() && expandStore[p]) {
            expandStore[p] = false;
        }
        shrinkRecursively(p);
        erase_entrylist(s, e);
        redraw_line(parent, parent + 1);
        return;
    }

    return;
}
void Tree::goto_(const nvim::Array &args)
{
    const int l = ctx.cursor - 1;

    if (args.size()>0) {
        string dest = args.at(0).as_string();

        if (dest=="parent") {
            int p = find_parent(l);
            api->async_win_set_cursor(0, {p+1, 0});
        }
    }
    else {
    }
}
void Tree::execute_system(const nvim::Array &args)
{
    FileItem &cur = *m_fileitem[ctx.cursor - 1];
    string info = cur.p.string();
    api->call_function("tree#util#open", {info});
}
void Tree::toggle_ignored_files(const nvim::Array &args)
{
    cfg.show_ignored_files = !cfg.show_ignored_files;
    FileItem &root = *m_fileitem[0];
    changeRoot(root.p.string());
}
