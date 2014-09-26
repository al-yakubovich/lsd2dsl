#include "MainWindow.h"

#include "../DslWriter.h"
#include "../dictlsd/lsd.h"
#include "../dictlsd/tools.h"

#include <QTableView>
#include <QAbstractListModel>
#include <QMimeData>
#include <QStringList>
#include <QByteArray>
#include <QDataStream>
#include <QPixmap>
#include <QHeaderView>
#include <QSortFilterProxyModel>
#include <QDockWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QFileDialog>
#include <QProgressBar>
#include <QThread>

#include <vector>
#include <memory>
#include <algorithm>
#include <assert.h>

using namespace dictlsd;

class FileStream : public IRandomAccessStream {
    QFile _file;
public:
    FileStream(QString path) : _file(path) {
        if (!_file.open(QIODevice::ReadOnly)) {
            throw std::runtime_error("can't read file");
        }
    }
    virtual void readSome(void *dest, unsigned byteCount) {
        _file.read(static_cast<char*>(dest), byteCount);
    }
    virtual void seek(unsigned pos) {
        _file.seek(pos);
    }
    virtual unsigned tell() {
        return _file.pos();
    }
};

class Dictionary {
    FileStream _stream;
    BitStreamAdapter _adapter;
    std::unique_ptr<LSDDictionary> _reader;
    QString _path;
    QString _fileName;
public:
    Dictionary(QString path)
        : _stream(path),
          _adapter(&_stream),
          _reader(new LSDDictionary(&_adapter)),
          _path(path)
    {
        _fileName = QFileInfo(path).fileName();
    }
    QString path() const {
        return _path;
    }
    QString fileName() const {
        return _fileName;
    }
    LSDDictionary const& reader() const {
        return *_reader;
    }
};

class LSDListModel : public QAbstractListModel {
    std::vector<std::unique_ptr<Dictionary>> _dicts;
    std::vector<QString> _columns;
public:
    LSDListModel() {
        _columns = {
            "",
            "File Name",
            "Name",
            "Source",
            "Target",
            "Entries",
            "Version"
        };
    }
    virtual Qt::DropActions supportedDropActions() const {
        return Qt::CopyAction | Qt::MoveAction;
    }
    virtual QStringList mimeTypes() const {
        QStringList types;
        types << "text/uri-list";
        return types;
    }
    virtual bool dropMimeData(const QMimeData *data, Qt::DropAction action, int, int, const QModelIndex &parent) {
        beginRemoveRows(parent, 0, _dicts.size() - 1);
        _dicts.clear();
        endRemoveRows();
        if (action == Qt::IgnoreAction)
            return true;
        for (QUrl fileUri : data->urls()) {
            QString path = fileUri.toLocalFile();
            try {                
                _dicts.emplace_back(new Dictionary(path));
            } catch(std::exception& e) {
                QMessageBox::warning(nullptr, QString(e.what()), path);
            }
        }        
        beginInsertRows(parent, 0, _dicts.size() - 1);
        endInsertRows();
        return true;
    }
    virtual Qt::ItemFlags flags(const QModelIndex &index) const {
        return QAbstractItemModel::flags(index) | (index.isValid() ? Qt::NoItemFlags : Qt::ItemIsDropEnabled);
    }
    virtual int rowCount(const QModelIndex &) const {
        return _dicts.size();
    }
    std::vector<std::unique_ptr<Dictionary>>& dicts() {
        return _dicts;
    }
    QString printLanguage(int code) const {
        return QString::fromStdString(toUtf8(langFromCode(code)));
    }
    virtual QVariant data(const QModelIndex &index, int role) const {
        int row = index.row();        
        auto&& reader = _dicts.at(row)->reader();

        if (role == Qt::DecorationRole && index.column() == 0) {
            QPixmap icon;
            auto&& rawBytes = reader.icon();
            icon.loadFromData(rawBytes.data(), rawBytes.size());
            return QVariant(icon);
        }

        LSDHeader const& header = reader.header();
        int source = header.sourceLanguage;
        int target = header.targetLanguage;

        if (role == Qt::DisplayRole) {
            switch(index.column()) {
            case 1: return _dicts.at(row)->fileName();
            case 2: return QString::fromStdString(toUtf8(reader.name()));
            case 3: return QString("%1 (%2)").arg(source).arg(printLanguage(source));
            case 4: return QString("%1 (%2)").arg(target).arg(printLanguage(target));
            case 5: return header.entriesCount;
            case 6: return QString("%1").arg(header.version, 1, 16);
            }
        }
        return QVariant();
    }
    virtual int columnCount(const QModelIndex &) const {
        return _columns.size();
    }
    virtual QVariant headerData(int section, Qt::Orientation orientation, int role) const {
        if (section == -1)
            return QVariant();
        if (role == Qt::DisplayRole && orientation == Qt::Horizontal) {
            return _columns.at(section);
        }
        return QVariant();
    }
    virtual bool removeRows(int row, int count, const QModelIndex &parent) {
        assert(count == 1); (void)count;
        beginRemoveRows(parent, row, row);
        _dicts.erase(begin(_dicts) + row);
        endRemoveRows();
        return true;
    }
};

class ConvertWithProgress : public QObject {
    Q_OBJECT
    std::vector<Dictionary*> _dicts;
    QString _outDir;
signals:
    void statusUpdated(int percent);
    void nextDictionary(QString name);
    void done();
public:
    ConvertWithProgress(std::vector<Dictionary*> dicts, QString outDir)
        : _dicts(dicts), _outDir(outDir) { }
public slots:
    void start() {
        for (Dictionary* dict : _dicts) {
            emit nextDictionary(dict->fileName());            
            writeDSL(&dict->reader(), dict->fileName().toStdString(), _outDir.toStdString(), [&](int percent, std::string) {
                emit statusUpdated(percent);
            });
            emit statusUpdated(100);
        }
        emit done();
    }
};

void MainWindow::convert(bool selectedOnly) {
    QString dir = QFileDialog::getExistingDirectory(this, "Select directory to save DSL");
    if (dir.isEmpty())
        return;
    std::vector<Dictionary*> dicts;
    if (selectedOnly) {
        for (auto index : _tableView->selectionModel()->selectedRows()) {
            dicts.push_back(_model->dicts()[index.row()].get());
        }
    } else {
        for (auto& dict : _model->dicts()) {
            dicts.push_back(dict.get());
        }
    }

    _progress->setMaximum(dicts.size());
    _progress->setValue(0);    

    auto thread = new QThread();
    auto converter = new ConvertWithProgress(dicts, dir);
    converter->moveToThread(thread);

    connect(converter, &ConvertWithProgress::statusUpdated, this, [=](int percent) {
        _dictProgress->setValue(percent);
    });
    connect(converter, &ConvertWithProgress::nextDictionary, this, [=](QString name) {
        _progress->setValue(_progress->value() + 1);
        _dictProgress->setValue(0);
        _currentDict->setText("Decoding " + name + "...");
    });
    connect(converter, &ConvertWithProgress::done, this, [=] {
        _currentDict->setText("");
        _tableView->setEnabled(true);
        _convertAllButton->setEnabled(true);
        _convertSelectedButton->setEnabled(true);
    });

    _tableView->setEnabled(false);
    _convertAllButton->setEnabled(false);
    _convertSelectedButton->setEnabled(false);

    connect(thread, &QThread::started, converter, &ConvertWithProgress::start);
    connect(converter, &ConvertWithProgress::done, converter, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setMinimumWidth(1200);
    setMinimumHeight(800);

    setWindowTitle("lsd2dsl");

    auto form = new QFormLayout(this);
    auto totalLabel = new QLabel("0");
    auto selectedLabel = new QLabel("0");
    form->addRow("Total:", totalLabel);
    form->addRow("Selected:", selectedLabel);

    auto vbox = new QVBoxLayout(this);
    auto rightPanelWidget = new QWidget(this);
    rightPanelWidget->setLayout(vbox);
    vbox->addLayout(form);
    vbox->addWidget(_currentDict = new QLabel(this));
    vbox->addWidget(_dictProgress = new QProgressBar(this));
    vbox->addWidget(_progress = new QProgressBar(this));
    vbox->addWidget(_convertAllButton = new QPushButton("Convert all"));
    vbox->addWidget(_convertSelectedButton = new QPushButton("Convert selected"));
    vbox->addStretch(1);

    auto rightDock = new QDockWidget(this);
    rightDock->setTitleBarWidget(new QWidget());
    auto rightPanel = new QWidget(this);
    rightPanel->setMinimumWidth(300);
    rightPanel->setLayout(vbox);
    rightDock->setWidget(rightPanel);
    rightDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    addDockWidget(Qt::RightDockWidgetArea, rightDock);

    auto topDock = new QDockWidget(this, Qt::FramelessWindowHint);
    topDock->setTitleBarWidget(new QWidget());
    topDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    auto dragDropLabel = new QLabel("Drag and drop LSD files here");
    dragDropLabel->setMargin(5);
    topDock->setWidget(dragDropLabel);
    addDockWidget(Qt::TopDockWidgetArea, topDock);

    _tableView = new QTableView(this);
    _tableView->setDropIndicatorShown(true);
    _tableView->setAcceptDrops(true);
    auto proxy = new QSortFilterProxyModel;
    _model = new LSDListModel;
    proxy->setSourceModel(_model);
    _tableView->setModel(proxy);
    _tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    _tableView->setSortingEnabled(true);
    _tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    setCentralWidget(_tableView);

    _convertAllButton->setEnabled(false);
    _convertSelectedButton->setEnabled(false);

    connect(_tableView->selectionModel(), &QItemSelectionModel::selectionChanged, [=] {
        int count = _tableView->selectionModel()->selectedRows().size();
        _convertSelectedButton->setEnabled(count > 0);
        selectedLabel->setText(QString::number(count));
    });
    auto updateRowCount = [=]() {
        totalLabel->setText(QString::number(_model->rowCount(QModelIndex())));
        _convertAllButton->setEnabled(_model->rowCount(QModelIndex()) > 0);
    };
    connect(_model, &QAbstractItemModel::rowsInserted, updateRowCount);
    connect(_model, &QAbstractItemModel::rowsRemoved, updateRowCount);
    connect(_convertAllButton, &QPushButton::clicked, [=] { convert(false); });
    connect(_convertSelectedButton, &QPushButton::clicked, [=] { convert(true); });
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    if (!event->matches(QKeySequence::Delete))
        return;
    auto rows = _tableView->selectionModel()->selectedRows();
    for (int i = rows.size() - 1; i >= 0; --i) {
        _model->removeRow(rows[i].row());
    }
}

MainWindow::~MainWindow() { }

#include "MainWindow.moc"
