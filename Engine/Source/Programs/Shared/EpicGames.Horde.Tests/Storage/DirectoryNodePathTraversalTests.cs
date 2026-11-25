// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using EpicGames.Core;
using EpicGames.Horde.Storage;
using EpicGames.Horde.Storage.Nodes;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace EpicGames.Horde.Tests.Storage
{
	[TestClass]
	public class DirectoryNodePathTraversalTests
	{
		static readonly byte[] s_helloBytes = Encoding.UTF8.GetBytes("hello");

		// Names that are not a single, safe path component and must be rejected when deserialized.
		[TestMethod]
		[DataRow("../../out")]
		[DataRow("a/b")]
		[DataRow("a\\b")]
		[DataRow("/out")]
		[DataRow("C:\\out")]
		[DataRow("\\\\host\\share")]
		[DataRow("name:stream")]
		[DataRow(".")]
		[DataRow("..")]
		[DataRow("")]
		[DataRow("foo.")]
		[DataRow("foo ")]
		[DataRow("file.txt.")]
		public void RejectsInvalidEntryName(string name)
		{
			Assert.ThrowsExactly<InvalidDataException>(() => DirectoryNode.ValidateEntryName(name));
		}

		// Ordinary leaf names that must continue to pass, guarding against over-rejection.
		[TestMethod]
		[DataRow("file.txt")]
		[DataRow(".gitignore")]
		[DataRow(".config")]
		[DataRow("archive.tar.gz")]
		[DataRow("file.v1.2.bin")]
		[DataRow("a.b")]
		[DataRow("a..b")]
		[DataRow("My File.txt")]
		[DataRow("café.txt")]
		public void AcceptsValidLeafName(string name)
		{
			DirectoryNode.ValidateEntryName(name);
		}

		[TestMethod]
		public async Task RejectsTraversalFileNameOnExtractAsync()
		{
			await using MemoryBlobWriter blobWriter = new(new BlobSerializerOptions());

			DirectoryNode rootDir = new();
			rootDir.AddFile("../../out", FileEntryFlags.None, await ChunkedData.CreateAsync(blobWriter, s_helloBytes));
			IHashedBlobRef<DirectoryNode> handle = await blobWriter.WriteBlobAsync(rootDir);

			DirectoryInfo tempDir = CreateTempDir();
			try
			{
				await Assert.ThrowsExactlyAsync<InvalidDataException>(() => handle.ExtractAsync(tempDir, new ExtractOptions(), NullLogger.Instance, CancellationToken.None));
			}
			finally
			{
				DeleteTempDir(tempDir);
			}
		}

		[TestMethod]
		public async Task ExtractsValidNestedStructureAsync()
		{
			await using MemoryBlobWriter blobWriter = new(new BlobSerializerOptions());

			DirectoryNode subDir = new();
			subDir.AddFile("file.txt", FileEntryFlags.None, await ChunkedData.CreateAsync(blobWriter, s_helloBytes));
			IHashedBlobRef<DirectoryNode> subDirRef = await blobWriter.WriteBlobAsync(subDir);

			DirectoryNode rootDir = new();
			rootDir.AddDirectory(new DirectoryEntry("subdir", subDir.Length, subDirRef));
			IHashedBlobRef<DirectoryNode> handle = await blobWriter.WriteBlobAsync(rootDir);

			DirectoryInfo tempDir = CreateTempDir();
			try
			{
				await handle.ExtractAsync(tempDir, new ExtractOptions(), NullLogger.Instance, CancellationToken.None);

				byte[] outputData = await File.ReadAllBytesAsync(Path.Combine(tempDir.FullName, "subdir", "file.txt"));
				Assert.IsTrue(outputData.SequenceEqual(s_helloBytes));
			}
			finally
			{
				DeleteTempDir(tempDir);
			}
		}

		[TestMethod]
		public async Task RejectsTraversalDirectoryNameOnExtractAsync()
		{
			await using MemoryBlobWriter blobWriter = new(new BlobSerializerOptions());

			DirectoryNode subDir = new();
			subDir.AddFile("file.txt", FileEntryFlags.None, await ChunkedData.CreateAsync(blobWriter, s_helloBytes));
			IHashedBlobRef<DirectoryNode> subDirRef = await blobWriter.WriteBlobAsync(subDir);

			DirectoryNode rootDir = new();
			rootDir.AddDirectory(new DirectoryEntry("../../out", subDir.Length, subDirRef));
			IHashedBlobRef<DirectoryNode> handle = await blobWriter.WriteBlobAsync(rootDir);

			DirectoryInfo tempDir = CreateTempDir();
			try
			{
				await Assert.ThrowsExactlyAsync<InvalidDataException>(() => handle.ExtractAsync(tempDir, new ExtractOptions(), NullLogger.Instance, CancellationToken.None));
			}
			finally
			{
				DeleteTempDir(tempDir);
			}
		}

		[TestMethod]
		public async Task BackstopRejectsTraversalFileNameWithoutDeserializationAsync()
		{
			// Build the node in-process and extract it directly, so the name never passes through
			// DirectoryNodeConverter.Read. This isolates the write-site containment backstop. The backstop must
			// fail closed: extraction aborts and nothing is written outside the target directory. The surfaced
			// exception type is not asserted because the extraction pipeline may report the abort either as the
			// InvalidDataException or as a cancellation once the failing task cancels its siblings.
			await using MemoryBlobWriter blobWriter = new(new BlobSerializerOptions());

			DirectoryInfo tempDir = CreateTempDir();
			string escapeName = $"{Guid.NewGuid():N}.out";
			FileInfo escapee = new(Path.Combine(tempDir.FullName, "..", escapeName));

			DirectoryNode rootDir = new();
			rootDir.AddFile($"../{escapeName}", FileEntryFlags.None, await ChunkedData.CreateAsync(blobWriter, s_helloBytes));

			try
			{
				bool aborted = false;
				try
				{
					await rootDir.ExtractAsync(tempDir, new ExtractOptions(), NullLogger.Instance, CancellationToken.None);
				}
				catch (InvalidDataException)
				{
					aborted = true;
				}
				catch (OperationCanceledException)
				{
					aborted = true;
				}

				Assert.IsTrue(aborted);
				Assert.IsFalse(File.Exists(escapee.FullName));
			}
			finally
			{
				escapee.Refresh();
				if (escapee.Exists)
				{
					escapee.Delete();
				}
				DeleteTempDir(tempDir);
			}
		}

		[TestMethod]
		public async Task RejectsTraversalNameOnSyncAsync()
		{
			DirectoryInfo tempDir = CreateTempDir();
			try
			{
				Workspace ws = await Workspace.CreateAsync(new DirectoryReference(tempDir), NullLogger.Instance);
				await using MemoryBlobWriter blobWriter = new(new BlobSerializerOptions());

				DirectoryNode rootDir = new();
				rootDir.AddFile("../../out", FileEntryFlags.None, await ChunkedData.CreateAsync(blobWriter, s_helloBytes));
				IHashedBlobRef<DirectoryNode> state = await blobWriter.WriteBlobAsync(rootDir);

				await Assert.ThrowsExactlyAsync<InvalidDataException>(() => ws.SyncAsync(WorkspaceLayerId.Default, state, null));
			}
			finally
			{
				DeleteTempDir(tempDir);
			}
		}

		[TestMethod]
		public async Task SyncsValidNestedTreeAsync()
		{
			DirectoryInfo tempDir = CreateTempDir();
			try
			{
				Workspace ws = await Workspace.CreateAsync(new DirectoryReference(tempDir), NullLogger.Instance);
				await using MemoryBlobWriter blobWriter = new(new BlobSerializerOptions());

				DirectoryNode subDir = new();
				subDir.AddFile("file.txt", FileEntryFlags.None, await ChunkedData.CreateAsync(blobWriter, s_helloBytes));
				IHashedBlobRef<DirectoryNode> subDirRef = await blobWriter.WriteBlobAsync(subDir);

				DirectoryNode rootDir = new();
				rootDir.AddDirectory(new DirectoryEntry("subdir", subDir.Length, subDirRef));
				IHashedBlobRef<DirectoryNode> state = await blobWriter.WriteBlobAsync(rootDir);

				await ws.SyncAsync(WorkspaceLayerId.Default, state, null);

				byte[] outputData = await File.ReadAllBytesAsync(Path.Combine(tempDir.FullName, "subdir", "file.txt"));
				Assert.IsTrue(outputData.SequenceEqual(s_helloBytes));
			}
			finally
			{
				DeleteTempDir(tempDir);
			}
		}

		static DirectoryInfo CreateTempDir()
		{
			DirectoryInfo tempDir = new(Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N")));
			tempDir.Create();
			return tempDir;
		}

		static void DeleteTempDir(DirectoryInfo tempDir)
		{
			tempDir.Refresh();
			if (tempDir.Exists)
			{
				tempDir.Delete(true);
			}
		}
	}
}
