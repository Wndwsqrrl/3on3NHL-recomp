using System.Buffers.Binary;
using System.Text;

namespace Nhl3on3Launcher;

/// <summary>Reads files out of an Xbox 360 STFS package (LIVE / PIRS / CON).
/// Block addressing follows Xenia's StfsContainerDevice.</summary>
public sealed class StfsPackage : IDisposable
{
    const int BlockSize = 0x1000;
    const int PerLevel = 0xAA;  // entries per hash table

    public sealed record Entry(string Path, bool IsDirectory, bool Contiguous, int Blocks, int StartBlock, uint Size);

    readonly FileStream _stream;
    readonly long _dataStart;
    readonly int _tablesPerHash;
    readonly int _step0;
    readonly int _tableBlocks;
    readonly int _tableStart;

    public string Magic { get; }
    public uint TitleId { get; }
    public string DisplayName { get; }

    StfsPackage(FileStream stream, byte[] head)
    {
        _stream = stream;
        Magic = Encoding.ASCII.GetString(head, 0, 4);
        uint headerSize = BinaryPrimitives.ReadUInt32BigEndian(head.AsSpan(0x340));
        TitleId = BinaryPrimitives.ReadUInt32BigEndian(head.AsSpan(0x360));
        DisplayName = Encoding.BigEndianUnicode.GetString(head, 0x411, 0x80).Split('\0')[0];

        var vd = head.AsSpan(0x379, 0x24);
        bool readOnly = (vd[2] & 1) != 0;
        _tableBlocks = BinaryPrimitives.ReadUInt16LittleEndian(vd[3..]);
        _tableStart = U24Le(vd[5..]);
        _dataStart = (headerSize + 0xFFF) & ~0xFFFL;
        bool singleTable = readOnly || ((headerSize + 0xFFF) & 0xB000) == 0xB000;
        _tablesPerHash = singleTable ? 1 : 2;
        _step0 = singleTable ? 0xAB : 0xAC;
    }

    /// <summary>Opens a package, or returns null if the file is not an STFS package.</summary>
    public static StfsPackage? TryOpen(string path)
    {
        FileStream? stream = null;
        try
        {
            stream = File.OpenRead(path);
            if (stream.Length < BlockSize) { stream.Dispose(); return null; }
            var head = new byte[BlockSize];
            stream.ReadExactly(head);
            string magic = Encoding.ASCII.GetString(head, 0, 4);
            if (magic is not ("LIVE" or "PIRS" or "CON ")) { stream.Dispose(); return null; }
            return new StfsPackage(stream, head);
        }
        catch (IOException) { stream?.Dispose(); return null; }
        catch (UnauthorizedAccessException) { stream?.Dispose(); return null; }
    }

    static int U24Le(ReadOnlySpan<byte> b) => b[0] | (b[1] << 8) | (b[2] << 16);
    static int U24Be(ReadOnlySpan<byte> b) => (b[0] << 16) | (b[1] << 8) | b[2];

    long BlockOffset(int index)
    {
        long block = index;
        long levelBase = PerLevel;
        for (int i = 0; i < 3; i++)
        {
            block += ((index + levelBase) / levelBase) * _tablesPerHash;
            if (index < levelBase) break;
            levelBase *= PerLevel;
        }
        return _dataStart + block * BlockSize;
    }

    long HashTableOffset(int index)
    {
        long block;
        if (index < PerLevel)
        {
            block = 0;
        }
        else
        {
            block = (long)(index / PerLevel) * _step0;
            block += ((index / 0x70E4) + 1) * _tablesPerHash;
            if (index >= 0x70E4) block += _tablesPerHash;
        }
        return _dataStart + block * BlockSize;
    }

    int NextBlock(int index)
    {
        Span<byte> b = stackalloc byte[3];
        _stream.Position = HashTableOffset(index) + (index % PerLevel) * 0x18 + 0x15;
        _stream.ReadExactly(b);
        return U24Be(b);
    }

    void ReadBlock(int index, Span<byte> buffer)
    {
        _stream.Position = BlockOffset(index);
        _stream.ReadExactly(buffer);
    }

    IEnumerable<int> Chain(int start, int count, bool contiguous)
    {
        int index = start;
        for (int i = 0; i < count; i++)
        {
            yield return index;
            index = contiguous ? index + 1 : NextBlock(index);
        }
    }

    public List<Entry> ReadEntries()
    {
        var raw = new List<(string Name, bool Dir, bool Contig, int Blocks, int Start, short Parent, uint Size)>();
        var block = new byte[BlockSize];
        foreach (int index in Chain(_tableStart, _tableBlocks, false))
        {
            ReadBlock(index, block);
            for (int pos = 0; pos < BlockSize; pos += 0x40)
            {
                var e = block.AsSpan(pos, 0x40);
                byte flags = e[0x28];
                if ((flags & 0x3F) == 0) continue;
                raw.Add((Encoding.Latin1.GetString(e[..(flags & 0x3F)]),
                         (flags & 0x80) != 0, (flags & 0x40) != 0,
                         U24Le(e[0x29..]), U24Le(e[0x2F..]),
                         BinaryPrimitives.ReadInt16BigEndian(e[0x32..]),
                         BinaryPrimitives.ReadUInt32BigEndian(e[0x34..])));
            }
        }

        string PathOf(int i)
        {
            var parts = new List<string>();
            while (i != -1)
            {
                parts.Add(raw[i].Name);
                i = raw[i].Parent;
            }
            parts.Reverse();
            return System.IO.Path.Combine(parts.ToArray());
        }

        return raw.Select((r, i) => new Entry(PathOf(i), r.Dir, r.Contig, r.Blocks, r.Start, r.Size)).ToList();
    }

    /// <summary>Extracts every file into outDir, reporting (bytesDone, bytesTotal).</summary>
    public void ExtractAll(string outDir, IProgress<(long Done, long Total)>? progress, CancellationToken cancel)
    {
        var entries = ReadEntries();
        long total = entries.Where(e => !e.IsDirectory).Sum(e => (long)e.Size);
        long done = 0;
        var block = new byte[BlockSize];
        foreach (var entry in entries)
        {
            cancel.ThrowIfCancellationRequested();
            string dest = System.IO.Path.Combine(outDir, entry.Path);
            if (entry.IsDirectory)
            {
                Directory.CreateDirectory(dest);
                continue;
            }
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(dest)!);
            using var output = File.Create(dest);
            long remaining = entry.Size;
            foreach (int index in Chain(entry.StartBlock, entry.Blocks, entry.Contiguous))
            {
                if (remaining <= 0) break;
                ReadBlock(index, block);
                int n = (int)Math.Min(remaining, BlockSize);
                output.Write(block, 0, n);
                remaining -= n;
                done += n;
            }
            progress?.Report((done, total));
        }
    }

    public void Dispose() => _stream.Dispose();
}
