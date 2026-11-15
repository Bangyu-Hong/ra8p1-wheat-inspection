$ErrorActionPreference = 'Stop'

$source = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Linq;
using System.Text;

public sealed class TjcV6FontMaker
{
    private sealed class Glyph
    {
        public ushort CodePoint;
        public byte Width;
        public byte[] Data;
    }

    public static void Create(string outputPath, string resourceName, string systemFontName,
        float requestedPixelSize, byte characterHeight, string characters)
    {
        List<ushort> codePoints = characters
            .Where(c => c != '\r' && c != '\n')
            .Select(c => (ushort)c)
            .Distinct()
            .OrderBy(c => c)
            .ToList();

        if (codePoints.Count == 0)
            throw new InvalidOperationException("The character set is empty.");

        List<ushort> sizingCodePoints = codePoints
            .Where(cp => cp >= 0x3400)
            .ToList();
        if (sizingCodePoints.Count == 0)
            sizingCodePoints = codePoints;

        using (Font font = FindFittingFont(systemFontName, requestedPixelSize, characterHeight, sizingCodePoints))
        {
            Console.WriteLine("{0}: family={1}, pixelSize={2}, height={3}",
                Path.GetFileName(outputPath), font.FontFamily.Name, font.Size, characterHeight);
            int verticalOffset = CalculateVerticalOffset(font, characterHeight, sizingCodePoints);
            List<Glyph> glyphs = codePoints
                .Select(cp => RenderGlyph(cp, font, characterHeight, verticalOffset))
                .ToList();

            WriteFont(outputPath, resourceName, characterHeight, glyphs);
        }
    }

    private static Graphics CreateGraphics(Bitmap bitmap)
    {
        Graphics graphics = Graphics.FromImage(bitmap);
        graphics.CompositingQuality = CompositingQuality.HighQuality;
        graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
        graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        graphics.TextRenderingHint = TextRenderingHint.SingleBitPerPixelGridFit;
        graphics.TextContrast = 0;
        return graphics;
    }

    private static StringFormat CreateStringFormat()
    {
        StringFormat format = (StringFormat)StringFormat.GenericTypographic.Clone();
        format.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces |
            StringFormatFlags.NoWrap | StringFormatFlags.NoFontFallback;
        return format;
    }

    private static Font FindFittingFont(string name, float requestedSize, int height,
        IList<ushort> codePoints)
    {
        for (float size = requestedSize; size >= 6.0f; size -= 0.5f)
        {
            Font candidate = new Font(name, size, FontStyle.Regular, GraphicsUnit.Pixel);
            int minY;
            int maxY;
            GetGlobalVerticalBounds(candidate, codePoints, out minY, out maxY);
            if (maxY < minY || maxY - minY + 1 <= height - 1)
                return candidate;
            candidate.Dispose();
        }

        throw new InvalidOperationException("The selected font cannot fit the requested height.");
    }

    private static int CalculateVerticalOffset(Font font, int height, IList<ushort> codePoints)
    {
        int minY;
        int maxY;
        GetGlobalVerticalBounds(font, codePoints, out minY, out maxY);
        if (maxY < minY)
            return 0;
        int inkHeight = maxY - minY + 1;
        return ((height - inkHeight) / 2) - minY;
    }

    private static void GetGlobalVerticalBounds(Font font, IList<ushort> codePoints,
        out int minY, out int maxY)
    {
        minY = Int32.MaxValue;
        maxY = Int32.MinValue;
        int canvasHeight = Math.Max(96, (int)Math.Ceiling(font.Size * 4));

        using (Bitmap bitmap = new Bitmap(160, canvasHeight))
        using (Graphics graphics = CreateGraphics(bitmap))
        using (StringFormat format = CreateStringFormat())
        using (Brush brush = new SolidBrush(Color.Black))
        {
            foreach (ushort cp in codePoints)
            {
                graphics.Clear(Color.Transparent);
                graphics.DrawString(Char.ConvertFromUtf32(cp), font, brush, 16, 0, format);
                Rectangle bounds = FindAlphaBounds(bitmap);
                if (bounds.IsEmpty)
                    continue;
                minY = Math.Min(minY, bounds.Top);
                maxY = Math.Max(maxY, bounds.Bottom - 1);
            }
        }
    }

    private static Glyph RenderGlyph(ushort codePoint, Font font, byte height, int verticalOffset)
    {
        string text = Char.ConvertFromUtf32(codePoint);
        int measuredWidth;

        using (Bitmap measureBitmap = new Bitmap(1, 1))
        using (Graphics graphics = CreateGraphics(measureBitmap))
        using (StringFormat format = CreateStringFormat())
        {
            measuredWidth = Math.Max(1, (int)Math.Ceiling(
                graphics.MeasureString(text, font, new PointF(0, 0), format).Width));
        }

        int tempWidth = Math.Min(255, Math.Max(64, measuredWidth + 32));
        using (Bitmap temp = new Bitmap(tempWidth, height))
        using (Graphics graphics = CreateGraphics(temp))
        using (StringFormat format = CreateStringFormat())
        using (Brush brush = new SolidBrush(Color.Black))
        {
            graphics.Clear(Color.Transparent);
            graphics.DrawString(text, font, brush, 16, verticalOffset, format);
            Rectangle ink = FindAlphaBounds(temp);

            int width = Math.Min(255, Math.Max(measuredWidth, ink.IsEmpty ? measuredWidth : ink.Width + 2));
            Bitmap glyphBitmap = new Bitmap(width, height);
            using (Graphics glyphGraphics = Graphics.FromImage(glyphBitmap))
            {
                glyphGraphics.Clear(Color.Transparent);
                if (!ink.IsEmpty)
                {
                    int destinationX = Math.Max(0, (width - ink.Width) / 2);
                    glyphGraphics.DrawImage(temp,
                        new Rectangle(destinationX, 0, ink.Width, height),
                        new Rectangle(ink.Left, 0, ink.Width, height),
                        GraphicsUnit.Pixel);
                }
            }

            byte[] encoded = EncodeBitmap(glyphBitmap);
            glyphBitmap.Dispose();
            return new Glyph { CodePoint = codePoint, Width = (byte)width, Data = encoded };
        }
    }

    private static Rectangle FindAlphaBounds(Bitmap bitmap)
    {
        int left = bitmap.Width;
        int top = bitmap.Height;
        int right = -1;
        int bottom = -1;

        for (int y = 0; y < bitmap.Height; y++)
        {
            for (int x = 0; x < bitmap.Width; x++)
            {
                if (bitmap.GetPixel(x, y).A == 0)
                    continue;
                left = Math.Min(left, x);
                top = Math.Min(top, y);
                right = Math.Max(right, x);
                bottom = Math.Max(bottom, y);
            }
        }

        return right < left ? Rectangle.Empty :
            Rectangle.FromLTRB(left, top, right + 1, bottom + 1);
    }

    private static byte[] EncodeBitmap(Bitmap bitmap)
    {
        List<byte> pixels = new List<byte>(bitmap.Width * bitmap.Height);
        for (int y = 0; y < bitmap.Height; y++)
        {
            for (int x = 0; x < bitmap.Width; x++)
                pixels.Add(bitmap.GetPixel(x, y).A >= 128 ? (byte)1 : (byte)0);
        }

        List<byte> data = new List<byte>();
        data.Add(1);
        int position = 0;
        while (position < pixels.Count)
        {
            byte color = pixels[position];
            int repetitions = 1;
            while (position + repetitions < pixels.Count &&
                pixels[position + repetitions] == color && repetitions < 31)
                repetitions++;
            position += repetitions;

            data.Add(color == 0 ? RepeatedTransparent(repetitions) : RepeatedBlack(repetitions));
        }

        return data.ToArray();
    }

    private static byte RepeatedBlack(int count)
    {
        return (byte)(0x20 | (count & 0x1F));
    }

    private static byte RepeatedTransparent(int count)
    {
        return (byte)(count & 0x1F);
    }

    private static void WriteFont(string outputPath, string name, byte height, IList<Glyph> glyphs)
    {
        string encodingName = "utf-8";
        byte[] nameBytes = Encoding.ASCII.GetBytes(name);
        byte[] encodingBytes = Encoding.ASCII.GetBytes(encodingName);
        int descriptionLength = nameBytes.Length + encodingBytes.Length;

        List<byte> index = new List<byte>();
        List<byte> characterData = new List<byte>();
        uint dataOffset = (uint)(glyphs.Count * 10);

        foreach (Glyph glyph in glyphs)
        {
            index.AddRange(BitConverter.GetBytes(glyph.CodePoint));
            index.Add(glyph.Width);
            index.Add(0);
            index.Add(0);
            index.Add((byte)(dataOffset & 0xFF));
            index.Add((byte)((dataOffset >> 8) & 0xFF));
            index.Add((byte)((dataOffset >> 16) & 0xFF));
            index.AddRange(BitConverter.GetBytes((ushort)glyph.Data.Length));
            characterData.AddRange(glyph.Data);
            dataOffset += (uint)glyph.Data.Length;
        }

        uint variableDataLength = dataOffset + (uint)descriptionLength;
        List<byte> file = new List<byte>();
        file.Add(0x04);
        file.Add(0xFF);
        file.Add(0x00);
        file.Add(0x0A);
        file.Add(0x18);
        file.Add(0x02);
        file.Add(0x00);
        file.Add(height);
        file.Add(0xFF);
        file.Add(0xFF);
        file.Add(0x00);
        file.Add(0xFF);
        file.AddRange(BitConverter.GetBytes(glyphs.Count));
        file.Add(0x06);
        file.Add((byte)descriptionLength);
        file.AddRange(new byte[] { 0, 0 });
        file.AddRange(BitConverter.GetBytes(variableDataLength));
        file.AddRange(BitConverter.GetBytes((uint)44));
        file.Add(0xFF);
        file.Add(0x00);
        file.Add(0x01);
        file.Add(0x01);
        file.Add((byte)nameBytes.Length);
        file.Add(0x00);
        file.AddRange(new byte[] { 0, 0 });
        file.AddRange(BitConverter.GetBytes((uint)65536));
        file.AddRange(new byte[] { 0, 0, 0, 0 });
        file.AddRange(nameBytes);
        file.AddRange(encodingBytes);
        file.AddRange(index);
        file.AddRange(characterData);

        File.WriteAllBytes(outputPath, file.ToArray());
    }
}
'@

Add-Type -TypeDefinition $source -ReferencedAssemblies System.Drawing

$fontDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$labels = [IO.File]::ReadAllText((Join-Path $fontDir 'font_charset_labels.txt'), [Text.Encoding]::UTF8)
$digits = [IO.File]::ReadAllText((Join-Path $fontDir 'font_charset_digits.txt'), [Text.Encoding]::UTF8)

$fontFamily = 'Microsoft YaHei UI'
$installedFamily = New-Object Drawing.FontFamily($fontFamily)
$installedFamily.Dispose()

[TjcV6FontMaker]::Create((Join-Path $fontDir 'WHEAT_14px_utf8.zi'), 'WHEAT14', $fontFamily, 14, 14, $labels)
[TjcV6FontMaker]::Create((Join-Path $fontDir 'WHEAT_16px_utf8.zi'), 'WHEAT16', $fontFamily, 16, 16, $labels)
[TjcV6FontMaker]::Create((Join-Path $fontDir 'WHEAT_18px_utf8.zi'), 'WHEAT18', $fontFamily, 18, 18, $labels)
[TjcV6FontMaker]::Create((Join-Path $fontDir 'WHEAT_20px_utf8.zi'), 'WHEAT20', $fontFamily, 20, 20, $labels)
[TjcV6FontMaker]::Create((Join-Path $fontDir 'WHEAT_24px_digits_utf8.zi'), 'WHEAT24D', $fontFamily, 24, 24, $digits)

Get-Item (Join-Path $fontDir 'WHEAT_14px_utf8.zi'),
    (Join-Path $fontDir 'WHEAT_16px_utf8.zi'),
    (Join-Path $fontDir 'WHEAT_18px_utf8.zi'),
    (Join-Path $fontDir 'WHEAT_20px_utf8.zi'),
    (Join-Path $fontDir 'WHEAT_24px_digits_utf8.zi') |
    Select-Object Name, Length, LastWriteTime
